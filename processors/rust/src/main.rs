#![recursion_limit = "512"]
use processor_rust::{
    ring,
    socket::{Receiver, RxStats, Status},
    *,
};
use serde_json::{json, Value};
use std::{
    env,
    fs::OpenOptions,
    io::{self, Write},
    net::Ipv4Addr,
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
    thread,
};
static INTERRUPTED: AtomicBool = AtomicBool::new(false);
extern "C" fn interrupt(_: libc::c_int) {
    INTERRUPTED.store(true, Ordering::Relaxed);
}
#[derive(Clone)]
struct Options {
    group: Ipv4Addr,
    interface: Ipv4Addr,
    port: u16,
    unit: u8,
    samples: usize,
    warmup: u64,
    ring_slots: usize,
    gap_slots: usize,
    max_live_orders: usize,
    idle_ms: u32,
    receive_buffer: i32,
    loss: LossPolicy,
    continuous: bool,
    output: Option<PathBuf>,
}
const HELP:&str="Usage: processor_rust [options]\n  --group IPv4 (239.255.0.1)\n  --interface IPv4 (127.0.0.1)\n  --port N (30001; 0 chooses an ephemeral port)\n  --unit N (1)\n  --messages N (1000000)\n  --warmup-messages N (180)\n  --ring-slots N (4096; power of two)\n  --gap-slots N (4096; power of two)\n  --max-live-orders N (1000000; 1..100000000 simultaneously live orders)\n  --receive-buffer N (4194304)\n  --idle-timeout-ms N (3000)\n  --on-loss stop|continue (stop)\n  --continuous\n  --output PATH (never overwritten)\n  --help\n\nTwo threads: recvmsg -> timestamped packet ring -> PITCH/L2 updates.\nCLOCK_MONOTONIC measures recvmsg return to each message's L2 update.\nControls and warmup are excluded; ring wait and earlier packet messages are included.\n";
fn number(value: &str, max: u64) -> Result<u64, String> {
    if value.is_empty() || !value.bytes().all(|b| b.is_ascii_digit()) {
        return Err(format!("Invalid integer {}", value));
    }
    let n = value.parse::<u64>().map_err(|e| e.to_string())?;
    if n > max {
        return Err("Integer is out of range".into());
    }
    Ok(n)
}
fn options() -> Result<Options, String> {
    let mut o = Options {
        group: Ipv4Addr::new(239, 255, 0, 1),
        interface: Ipv4Addr::LOCALHOST,
        port: 30001,
        unit: 1,
        samples: 1000000,
        warmup: 180,
        ring_slots: 4096,
        gap_slots: 4096,
        max_live_orders: DEFAULT_MAX_ORDERS,
        idle_ms: 3000,
        receive_buffer: 4194304,
        loss: LossPolicy::Stop,
        continuous: false,
        output: None,
    };
    let mut args = env::args().skip(1);
    while let Some(key) = args.next() {
        if key == "--help" {
            print!("{}", HELP);
            std::process::exit(0);
        }
        if key == "--continuous" {
            o.continuous = true;
            continue;
        }
        let value = args
            .next()
            .ok_or_else(|| format!("Missing value for {}", key))?;
        match key.as_str() {
            "--group" => o.group = value.parse().map_err(|e| format!("Invalid group: {}", e))?,
            "--interface" => {
                o.interface = value
                    .parse()
                    .map_err(|e| format!("Invalid interface: {}", e))?
            }
            "--port" => o.port = number(&value, 65535)? as u16,
            "--unit" => o.unit = number(&value, 255)? as u8,
            "--messages" => o.samples = number(&value, 100000000)? as usize,
            "--warmup-messages" => o.warmup = number(&value, 100000000)?,
            "--ring-slots" => o.ring_slots = number(&value, 65536)? as usize,
            "--gap-slots" => o.gap_slots = number(&value, 65536)? as usize,
            "--max-live-orders" => {
                o.max_live_orders = number(&value, LARGEST_ORDER_LIMIT as u64)? as usize
            }
            "--idle-timeout-ms" => o.idle_ms = number(&value, 60000)? as u32,
            "--receive-buffer" => o.receive_buffer = number(&value, 256 * 1024 * 1024)? as i32,
            "--on-loss" => {
                o.loss = match value.as_str() {
                    "stop" => LossPolicy::Stop,
                    "continue" => LossPolicy::Continue,
                    _ => return Err("--on-loss must be stop or continue".into()),
                }
            }
            "--output" => o.output = Some(value.into()),
            _ => return Err(format!("Unknown option: {}", key)),
        }
    }
    if !o.group.is_multicast()
        || o.unit == 0
        || o.samples == 0
        || o.max_live_orders == 0
        || o.idle_ms == 0
        || o.receive_buffer == 0
        || !o.ring_slots.is_power_of_two()
        || !o.gap_slots.is_power_of_two()
    {
        return Err(
            "Need multicast group, nonzero unit/messages/timeout/buffer and power-of-two queues"
                .into(),
        );
    }
    if o.interface.is_unspecified()
        || o.interface.is_multicast()
        || o.interface == Ipv4Addr::BROADCAST
    {
        return Err("Need a concrete unicast --interface address".into());
    }
    if o.output.as_ref().is_some_and(|p| p.exists()) {
        return Err("Report already exists".into());
    }
    Ok(o)
}
struct Finish(Arc<AtomicBool>);
impl Drop for Finish {
    fn drop(&mut self) {
        self.0.store(true, Ordering::Release);
    }
}
fn accept(status: Status, stats: &RxStats, o: &Options) -> Result<bool, String> {
    match status {
        Status::Idle => {
            if !o.continuous
                && stats.packets > 0
                && monotonic_ns() - stats.last_rx >= o.idle_ms as u64 * 1_000_000
            {
                Err("Receive idle timeout before run completion".into())
            } else {
                Ok(false)
            }
        }
        Status::Truncated => {
            if o.loss == LossPolicy::Stop {
                Err("UDP datagram exceeds the 1472-byte feed limit".into())
            } else {
                Ok(false)
            }
        }
        Status::Packet => {
            if stats.kernel_drops > 0 && o.loss == LossPolicy::Stop {
                Err("Kernel socket queue reported packet loss".into())
            } else {
                Ok(true)
            }
        }
    }
}
fn receive_loop(
    mut receiver: Receiver,
    mut producer: ring::Producer<Received>,
    o: Options,
    stop: Arc<AtomicBool>,
    finished: Arc<AtomicBool>,
) -> RxStats {
    let _finish = Finish(finished);
    let mut stats = RxStats::default();
    let mut scratch = Received::default();
    let result = (|| -> Result<(), String> {
        while !stop.load(Ordering::Relaxed) && !INTERRUPTED.load(Ordering::Relaxed) {
            {
                if let Some(mut slot) = producer.reserve() {
                    let status = receiver.receive(&mut slot, &mut stats)?;
                    if matches!(status, Status::Idle)
                        && (stop.load(Ordering::Relaxed) || INTERRUPTED.load(Ordering::Relaxed))
                    {
                        break;
                    }
                    if accept(status, &stats, &o)? {
                        slot.dropped_before =
                            stats.kernel_drops + stats.ring_drops + stats.truncated;
                        stats.high_watermark = stats.high_watermark.max(slot.commit());
                    }
                    continue;
                }
            }
            let status = receiver.receive(&mut scratch, &mut stats)?;
            if matches!(status, Status::Idle)
                && (stop.load(Ordering::Relaxed) || INTERRUPTED.load(Ordering::Relaxed))
            {
                break;
            }
            if !accept(status, &stats, &o)? {
                continue;
            }
            if let Some(mut slot) = producer.reserve() {
                scratch.dropped_before = stats.kernel_drops + stats.ring_drops + stats.truncated;
                *slot = scratch;
                stats.high_watermark = stats.high_watermark.max(slot.commit());
            } else {
                stats.ring_drops += 1;
                if o.loss == LossPolicy::Stop {
                    return Err("Packet ring overflow".into());
                }
            }
        }
        Ok(())
    })();
    if let Err(error) = result {
        stats.error = error;
    }
    stats
}
#[derive(Default)]
struct QueueWait {
    packets: u64,
    maximum: u64,
    sum: u128,
}
impl QueueWait {
    fn record(&mut self, value: u64) {
        self.packets += 1;
        self.maximum = self.maximum.max(value);
        self.sum += value as u128;
    }
}
fn gap_json(g: &GapRange) -> Value {
    json!({"unit":g.unit,"first_sequence":g.first_sequence,"last_sequence":g.last_sequence,"message_count":g.message_count,"detected_rx_ns":g.detected_rx_ns})
}
fn run(o: Options) -> Result<bool, String> {
    // SAFETY: handlers only set a lock-free atomic flag and have C signal-handler ABI.
    unsafe {
        libc::signal(libc::SIGINT, interrupt as *const () as libc::sighandler_t);
        libc::signal(libc::SIGTERM, interrupt as *const () as libc::sighandler_t);
    }
    let mut processor = Processor::new(
        o.unit,
        o.warmup,
        o.samples,
        o.loss,
        monotonic_ns,
        o.gap_slots,
        o.max_live_orders,
    )?;
    let mut gaps = processor.take_gap_consumer().unwrap();
    let (producer, mut consumer) = ring::channel::<Received>(o.ring_slots)?;
    let receiver = Receiver::new(o.group, o.interface, o.port, o.receive_buffer, o.idle_ms)?;
    let port = receiver.port;
    let buffer = receiver.buffer;
    let resolution = clock_resolution_ns();
    let stop = Arc::new(AtomicBool::new(false));
    let finished = Arc::new(AtomicBool::new(false));
    let (worker_options, worker_stop, worker_finished) =
        (o.clone(), stop.clone(), finished.clone());
    let worker = thread::spawn(move || {
        receive_loop(
            receiver,
            producer,
            worker_options,
            worker_stop,
            worker_finished,
        )
    });
    println!(
        "READY port={} group={} interface={}",
        port, o.group, o.interface
    );
    io::stdout().flush().map_err(|e| e.to_string())?;
    let mut queue = QueueWait::default();
    let mut observed_drops = 0;
    let mut error = String::new();
    while !INTERRUPTED.load(Ordering::Relaxed) && (o.continuous || !processor.complete()) {
        if let Some(packet) = consumer.read() {
            queue.record(monotonic_ns() - packet.rx_ns);
            if packet.dropped_before != observed_drops {
                processor.note_loss();
                observed_drops = packet.dropped_before;
            }
            if let Err(e) = processor.process(&packet.bytes[..packet.len], packet.rx_ns) {
                error = e;
                break;
            }
            continue;
        }
        if finished.load(Ordering::Acquire) && consumer.is_empty() {
            break;
        }
        // Tight busy-wait matches the C++ baseline (no sleep or PAUSE hint).
    }
    stop.store(true, Ordering::Relaxed);
    let rx = worker.join().unwrap_or_else(|_| RxStats {
        error: "RX thread panicked".into(),
        ..RxStats::default()
    });
    if observed_drops != rx.kernel_drops + rx.ring_drops + rx.truncated {
        processor.note_loss();
    }
    if error.is_empty() {
        error = rx.error.clone();
    }
    let cancelled = INTERRUPTED.load(Ordering::Relaxed);
    if !o.continuous && cancelled && error.is_empty() {
        error = "Interrupted before benchmark completion".into();
    }
    let valid = !o.continuous
        && error.is_empty()
        && processor.complete()
        && processor.book_current()
        && !processor.loss_seen()
        && processor.stats.sequence_errors == 0;
    let s = &processor.stats;
    let latency = processor.latency();
    let book = processor.book.snapshot();
    let mut gap_ranges = Vec::new();
    while let Some(g) = gaps.try_pop() {
        gap_ranges.push(gap_json(&g));
    }
    let levels:Vec<Value>=book.iter().map(|l|json!({"symbol":l.symbol,"side":l.side.to_string(),"price_1e4":l.price_1e4,"quantity":l.quantity,"orders":l.orders})).collect();
    let report = json!({
        "implementation":"rust-os","compiler":env!("PROCESSOR_COMPILER"),"build_type":env!("PROCESSOR_BUILD_TYPE"),
        "receive_api":"recvmsg","consumer_wait":"busy_spin","order_container":"std::collections::HashMap (RandomState)","level_container":"std::collections::BTreeMap","threads":2,
        "continuous":o.continuous,"loss_policy":if o.loss==LossPolicy::Stop {"stop"}else{"continue"},"benchmark_valid":valid,"complete":processor.complete(),"book_current":processor.book_current() && error.is_empty(),"error":error,
        "clock":"CLOCK_MONOTONIC","clock_resolution_ns":resolution,"timing_boundary":"recvmsg_return_to_message_l2_update",
        "group":o.group.to_string(),"interface":o.interface.to_string(),"max_live_orders":processor.book.max_orders(),"unit":o.unit,"target_samples":o.samples,"warmup_target":o.warmup,"warmup_messages":s.warmup_messages,
        "received_packets":rx.packets,"processed_packets":s.packets,"other_unit_packets":s.other_unit_packets,"pitch_messages":s.pitch_messages,"market_messages":s.market_messages,"applied_market_messages":s.applied_market_messages,"skipped_market_messages":s.skipped_market_messages,
        "received_payload_bytes":rx.bytes,"receive_span_ns":rx.last_rx-rx.first_rx,"receive_buffer_requested":o.receive_buffer,"receive_buffer_actual":buffer,"kernel_drop_counter_available":true,"kernel_dropped_packets":rx.kernel_drops,
        "ring_slots":o.ring_slots,"ring_high_watermark":rx.high_watermark,"ring_dropped_packets":rx.ring_drops,"truncated_packets":rx.truncated,"sequence_errors":s.sequence_errors,"missing_messages":s.missing_messages,"joined_midstream":s.joined_midstream,"out_of_order_packets":s.out_of_order_packets,"protocol_errors":s.protocol_errors,
        "gap_queue_capacity":o.gap_slots,"gap_ranges_enqueued":s.gap_ranges_enqueued,"gap_ranges_dropped":s.gap_ranges_dropped,"pending_gap_ranges":gap_ranges,
        "queue_wait":{"packets":queue.packets,"mean_ns":if queue.packets>0 {queue.sum as f64/queue.packets as f64}else{0.0},"max_ns":queue.maximum},
        "rx_to_l2":{"samples":latency.samples,"min_ns":latency.min_ns,"p50_ns":latency.p50_ns,"p99_ns":latency.p99_ns,"p99_9_ns":latency.p99_9_ns,"max_ns":latency.max_ns,"mean_ns":latency.mean_ns},
        "live_orders":processor.book.order_count(),"final_book":levels
    });
    let json = serde_json::to_string_pretty(&report).map_err(|e| e.to_string())?;
    if let Some(path) = &o.output {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(path)
            .map_err(|e| e.to_string())?;
        writeln!(file, "{}", json).map_err(|e| e.to_string())?;
    } else {
        println!("{}", json);
    }
    println!(
        "samples={} p50_ns={:?} p99_ns={:?} p99.9_ns={:?} benchmark_valid={}",
        latency.samples, latency.p50_ns, latency.p99_ns, latency.p99_9_ns, valid
    );
    println!(
        "L2 snapshot ({})\nSYMBOL SIDE PRICE QUANTITY ORDERS",
        if processor.book_current() {
            "current"
        } else {
            "incomplete"
        }
    );
    for l in book {
        println!(
            "{} {} {}.{:04} {} {}",
            l.symbol,
            l.side,
            l.price_1e4 / 10000,
            l.price_1e4 % 10000,
            l.quantity,
            l.orders
        );
    }
    if !error.is_empty() {
        eprintln!("processor_rust: {}", error);
    }
    Ok(valid || (o.continuous && cancelled && error.is_empty()))
}
fn main() {
    let result = options().and_then(run);
    match result {
        Ok(true) => (),
        Ok(false) => std::process::exit(1),
        Err(e) => {
            eprintln!("processor_rust: {}", e);
            std::process::exit(1)
        }
    }
}
