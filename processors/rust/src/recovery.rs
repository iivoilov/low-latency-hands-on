//! Per-unit sequencing and Cboe TCP recovery control. One owner, no shared book
//! locks. The normal path borrows the RX slot; only held messages are copied.
use crate::{
    advance_sequence, le, monotonic_ns, pitch, validate_pitch, Error, Processor, MAX_PAYLOAD,
};
use std::{
    env,
    io::{Read, Write},
    net::{SocketAddrV4, TcpStream},
    time::Duration,
};

#[derive(Clone)]
pub struct Options {
    pub grp: String,
    pub spin_server: String,
    pub buffer_messages: usize,
    pub max_gap: u32,
    pub timeout_ns: u64,
}
impl Default for Options {
    fn default() -> Self {
        Self {
            grp: String::new(),
            spin_server: String::new(),
            buffer_messages: 65536,
            max_gap: 10000,
            timeout_ns: 3_000_000_000,
        }
    }
}
#[derive(Default)]
pub struct Stats {
    pub gaps: u64,
    pub requests: u64,
    pub rejections: u64,
    pub duplicates: u64,
    pub replay_messages: u64,
    pub buffered_high_watermark: u64,
    pub spins: u64,
    pub timeouts: u64,
    pub buffer_overflows: u64,
    pub completed: u64,
    pub total_ns: u64,
    pub max_ns: u64,
}
fn distance(first: u32, last: u32) -> u32 {
    if last >= first {
        last - first
    } else {
        u32::MAX - first + last
    }
}
fn next(s: u32, n: u32) -> u32 {
    ((s as u64 - 1 + n as u64) % u32::MAX as u64 + 1) as u32
}
fn put(b: &mut [u8], at: usize, value: u64, size: usize) {
    b[at..at + size].copy_from_slice(&value.to_le_bytes()[..size]);
}
fn read32(b: &[u8], at: usize) -> u32 {
    le(&b[at..at + 4]) as u32
}

struct Session {
    unit: u8,
    stream: TcpStream,
    input: Vec<u8>,
    output: Vec<u8>,
    output_at: usize,
    logged_in: bool,
    last_rx: u64,
    last_tx: u64,
}
impl Session {
    fn new(endpoint: &str, unit: u8) -> Result<Self, Error> {
        let address: SocketAddrV4 = endpoint
            .parse()
            .map_err(|_| "Recovery endpoint must be IPv4:port")?;
        if address.port() == 0 {
            return Err("Invalid recovery port".into());
        }
        let stream = TcpStream::connect_timeout(&address.into(), Duration::from_secs(2))
            .map_err(|e| e.to_string())?;
        stream.set_nonblocking(true).map_err(|e| e.to_string())?;
        let now = monotonic_ns();
        let mut s = Self {
            unit,
            stream,
            input: Vec::with_capacity(65535),
            output: Vec::with_capacity(64),
            output_at: 0,
            logged_in: false,
            last_rx: now,
            last_tx: now,
        };
        let mut login = [b' '; 22];
        login[0] = 22;
        login[1] = 1;
        for (key, offset, size) in [
            ("PITCH_SESSION", 2, 4),
            ("PITCH_USERNAME", 6, 4),
            ("PITCH_PASSWORD", 12, 10),
        ] {
            let value =
                env::var(key).map_err(|_| format!("Set {} to a valid PITCH login field", key))?;
            if value.len() > size || !value.is_ascii() {
                return Err(format!("Invalid {} length/encoding", key));
            }
            login[offset..offset + value.len()].copy_from_slice(value.as_bytes());
        }
        s.send_message(&login)?;
        Ok(s)
    }
    fn writable(&self) -> bool {
        self.output.is_empty()
    }
    fn send_message(&mut self, message: &[u8]) -> Result<(), Error> {
        if !self.writable() {
            return Err("Recovery control output still pending".into());
        }
        self.output.resize(message.len() + 8, 0);
        put(&mut self.output, 0, (message.len() + 8) as u64, 2);
        self.output[2] = u8::from(!message.is_empty());
        self.output[8..].copy_from_slice(message);
        self.output_at = 0;
        self.flush()
    }
    fn flush(&mut self) -> Result<(), Error> {
        if self.output.is_empty() {
            return Ok(());
        }
        match self.stream.write(&self.output[self.output_at..]) {
            Ok(0) => return Err("Recovery TCP write closed".into()),
            Ok(n) => self.output_at += n,
            Err(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::Interrupted
                ) =>
            {
                return Ok(())
            }
            Err(e) => return Err(e.to_string()),
        }
        if self.output_at == self.output.len() {
            self.output.clear();
            self.last_tx = monotonic_ns();
        }
        Ok(())
    }
    fn tick(
        &mut self,
        now: u64,
        mut handle: impl FnMut(&[u8]) -> Result<(), Error>,
    ) -> Result<(), Error> {
        self.flush()?;
        for _ in 0..4 {
            let mut scratch = [0u8; 4096];
            let space = (65535 - self.input.len()).min(scratch.len());
            let n = match self.stream.read(&mut scratch[..space]) {
                Ok(0) => return Err("Recovery TCP connection closed".into()),
                Ok(n) => n,
                Err(e)
                    if matches!(
                        e.kind(),
                        std::io::ErrorKind::WouldBlock | std::io::ErrorKind::Interrupted
                    ) =>
                {
                    break
                }
                Err(e) => return Err(e.to_string()),
            };
            self.input.extend_from_slice(&scratch[..n]);
            self.last_rx = now;
            let mut at = 0;
            while self.input.len() - at >= 8 {
                let size = le(&self.input[at..at + 2]) as usize;
                if size < 8 {
                    return Err("Invalid recovery TCP frame length".into());
                }
                if self.input.len() - at < size {
                    break;
                }
                let frame = &self.input[at..at + size];
                if read32(frame, 4) != 0 || (frame[3] != 0 && frame[3] != self.unit) {
                    return Err("Invalid recovery TCP sequence/unit".into());
                }
                let mut offset = 8;
                for _ in 0..frame[2] {
                    if offset + 2 > size
                        || frame[offset] < 2
                        || offset + frame[offset] as usize > size
                    {
                        return Err("Invalid recovery TCP message framing".into());
                    }
                    let message = &frame[offset..offset + frame[offset] as usize];
                    if message[1] == 2 {
                        if message.len() < 3 || message[2] != b'A' || self.logged_in {
                            return Err("Recovery login rejected or repeated".into());
                        }
                        self.logged_in = true;
                    } else {
                        if !self.logged_in {
                            return Err("Recovery data before login".into());
                        }
                        handle(message)?;
                    }
                    offset += message.len();
                }
                if offset != size {
                    return Err("Trailing recovery TCP bytes".into());
                }
                at += size;
            }
            self.input.drain(..at);
        }
        if now - self.last_rx > 10_000_000_000 {
            return Err("Recovery TCP heartbeat timeout".into());
        }
        if self.logged_in && self.writable() && now - self.last_tx >= 1_000_000_000 {
            self.send_message(&[])?;
        }
        Ok(())
    }
}
#[derive(Clone)]
struct Entry {
    bytes: [u8; 255],
    length: u8,
    rx: u64,
}
impl Default for Entry {
    fn default() -> Self {
        Self {
            bytes: [0; 255],
            length: 0,
            rx: 0,
        }
    }
}
pub struct Recovery {
    options: Options,
    unit: u8,
    grp: Option<Box<Session>>,
    spin: Option<Box<Session>>,
    entries: Vec<Entry>,
    head: usize,
    buffered: usize,
    expected: u32,
    frontier: u32,
    seen: bool,
    pub active: bool,
    spinning: bool,
    spin_requested: bool,
    spin_receiving: bool,
    pending: bool,
    ack: bool,
    request_first: u32,
    request_end: u32,
    request_count: u16,
    image: u32,
    selected_image: u32,
    order_count: u32,
    received_orders: u32,
    started: u64,
    deadline: u64,
    spin_deadline: u64,
    last_tick: u64,
    request_times: [u64; 1500],
    pub stats: Stats,
}
impl Recovery {
    pub fn new(options: Options, unit: u8) -> Result<Self, Error> {
        if unit == 0
            || options.buffer_messages == 0
            || options.buffer_messages > 1_000_000
            || options.max_gap == 0
            || options.timeout_ns == 0
        {
            return Err("Invalid recovery bounds".into());
        }
        let grp = Some(Box::new(Session::new(&options.grp, unit)?));
        let spin = if options.spin_server.is_empty() {
            None
        } else {
            Some(Box::new(Session::new(&options.spin_server, unit)?))
        };
        Ok(Self {
            entries: vec![Entry::default(); options.buffer_messages],
            options,
            unit,
            grp,
            spin,
            head: 0,
            buffered: 0,
            expected: 1,
            frontier: 1,
            seen: false,
            active: false,
            spinning: false,
            spin_requested: false,
            spin_receiving: false,
            pending: false,
            ack: false,
            request_first: 0,
            request_end: 0,
            request_count: 0,
            image: 0,
            selected_image: 0,
            order_count: 0,
            received_orders: 0,
            started: 0,
            deadline: 0,
            spin_deadline: 0,
            last_tick: 0,
            request_times: [0; 1500],
            stats: Stats::default(),
        })
    }
    pub fn note_loss(&mut self, p: &mut Processor, now: u64) {
        if !self.active {
            self.active = true;
            self.started = now;
            self.stats.gaps += 1;
            p.begin_recovery();
        }
    }
    fn start_spin(&mut self, p: &mut Processor, now: u64) -> Result<(), Error> {
        // tick temporarily takes the session out to split mutable borrows.
        if self.options.spin_server.is_empty() {
            return Err("Gap cannot be recovered; no spin server configured".into());
        }
        if !self.spinning {
            self.note_loss(p, now);
            self.spinning = true;
            self.spin_requested = false;
            self.spin_receiving = false;
            self.spin_deadline = now + self.options.timeout_ns;
            self.stats.spins += 1;
        }
        Ok(())
    }
    fn finish(&mut self, p: &mut Processor, now: u64) {
        if self.active && !self.spinning && self.expected == self.frontier && self.buffered == 0 {
            self.active = false;
            p.finish_recovery();
            self.stats.completed += 1;
            let duration = now - self.started;
            self.stats.total_ns += duration;
            self.stats.max_ns = self.stats.max_ns.max(duration);
        }
    }
    fn drain(&mut self, p: &mut Processor, now: u64) -> Result<(), Error> {
        if self.spinning {
            return Ok(());
        }
        while self.entries[self.head].length != 0 {
            let entry = &self.entries[self.head];
            let mut packet = [0u8; MAX_PAYLOAD];
            let size = entry.length as usize + 8;
            put(&mut packet, 0, size as u64, 2);
            packet[2] = 1;
            packet[3] = self.unit;
            put(&mut packet, 4, self.expected as u64, 4);
            packet[8..size].copy_from_slice(&entry.bytes[..entry.length as usize]);
            p.process_sampled(&packet[..size], entry.rx, false)?;
            self.entries[self.head].length = 0;
            self.buffered -= 1;
            self.head = (self.head + 1) % self.entries.len();
            self.expected = next(self.expected, 1);
        }
        self.finish(p, now);
        Ok(())
    }
    fn rebase(&mut self, sequence: u32) -> Result<(), Error> {
        let skip = distance(self.expected, sequence);
        if skip > u32::MAX / 2 {
            return Err("Spin image precedes buffered suffix".into());
        }
        if skip as usize >= self.entries.len() {
            for e in &mut self.entries {
                e.length = 0;
            }
            self.buffered = 0;
            self.head = 0;
        } else {
            for _ in 0..skip {
                let e = &mut self.entries[self.head];
                if e.length != 0 {
                    e.length = 0;
                    self.buffered -= 1;
                }
                self.head = (self.head + 1) % self.entries.len();
            }
        }
        if distance(self.expected, self.frontier) < skip {
            self.frontier = sequence;
        }
        self.expected = sequence;
        Ok(())
    }
    fn control(
        &mut self,
        p: &mut Processor,
        m: &[u8],
        from_spin: bool,
        now: u64,
    ) -> Result<(), Error> {
        if !from_spin {
            if m[1] != 4
                || m.len() < 10
                || self.request_count == 0
                || m[2] != self.unit
                || read32(m, 3) != self.request_first
                || le(&m[7..9]) as u16 != self.request_count
            {
                return Err("Unexpected GRP response".into());
            }
            self.ack = true;
            if m[9] != b'A' {
                self.stats.rejections += 1;
                self.pending = false;
                if !self.spinning {
                    self.start_spin(p, now)?;
                }
            }
            return Ok(());
        }
        match m[1] {
            0x80 if m.len() >= 6 => self.image = read32(m, 2),
            0x82 if m.len() >= 11 && self.spin_requested && !self.spin_receiving => {
                if m[10] != b'A' {
                    return Err("Spin request rejected".into());
                }
                self.selected_image = read32(m, 2);
                if self.selected_image == 0 {
                    return Err("Unsequenced spin image boundary".into());
                }
                self.rebase(next(self.selected_image, 1))?;
                p.begin_spin()?;
                self.order_count = read32(m, 6);
                self.received_orders = 0;
                self.spin_receiving = true;
            }
            0x83 if m.len() >= 6 && self.spin_receiving => {
                if read32(m, 2) != self.selected_image || self.received_orders != self.order_count {
                    return Err("Incomplete spin image".into());
                }
                p.finish_spin(self.selected_image);
                self.spinning = false;
                self.spin_requested = false;
                self.spin_receiving = false;
                self.drain(p, now)?;
            }
            _ if self.spin_receiving => {
                p.spin_message(m)?;
                if matches!(m[1], 0x21 | 0x22 | 0x2f) {
                    self.received_orders += 1;
                }
            }
            _ => return Err("Unexpected spin server message".into()),
        }
        Ok(())
    }
    pub fn receive(
        &mut self,
        p: &mut Processor,
        payload: &[u8],
        rx: u64,
        replay: bool,
    ) -> Result<(), Error> {
        let now = monotonic_ns();
        validate_pitch(payload)?;
        if payload[3] != self.unit {
            if payload[3] == 0 && payload[2] == 0 && read32(payload, 4) == 0 {
                return Ok(());
            }
            if !replay {
                p.process(payload, rx)?;
            }
            return Ok(());
        }
        let sequence = read32(payload, 4);
        if sequence == 0 {
            if payload[2] != 0 {
                return Err("Unsequenced multicast data in recovery".into());
            }
            return Ok(());
        }
        if replay && !self.active {
            return Ok(());
        }
        if !replay && !self.seen && sequence != 1 {
            self.expected = sequence;
            self.frontier = sequence;
            self.start_spin(p, now)?;
        }
        if !replay {
            self.seen = true;
        }
        if !replay && !self.active && sequence == self.expected {
            p.process(payload, rx)?;
            self.expected = advance_sequence(sequence, payload[2]);
            self.frontier = self.expected;
            return Ok(());
        }
        let new_hole = distance(self.frontier, sequence);
        let end = advance_sequence(sequence, payload[2]);
        if !replay
            && distance(self.expected, end) <= u32::MAX / 2
            && distance(self.expected, end) > distance(self.expected, self.frontier)
        {
            self.frontier = end;
        }
        if !replay
            && distance(self.expected, sequence) > 0
            && distance(self.expected, sequence) <= u32::MAX / 2
        {
            self.note_loss(p, now);
            if new_hole <= u32::MAX / 2 && new_hole > self.options.max_gap {
                self.start_spin(p, now)?;
            }
        }
        let mut offset = 8;
        for i in 0..payload[2] {
            let m = &payload[offset..offset + payload[offset] as usize];
            pitch::decode(m)?;
            let seq = next(sequence, i as u32);
            let mut d = distance(self.expected, seq);
            if d > u32::MAX / 2 {
                self.stats.duplicates += 1;
                offset += m.len();
                continue;
            }
            if d as usize >= self.entries.len() {
                if self.spinning {
                    return Err("Recovery buffer exhausted during spin".into());
                }
                self.stats.buffer_overflows += 1;
                self.start_spin(p, now)?;
                self.rebase(seq)?;
                d = 0;
            }
            let index = (self.head + d as usize) % self.entries.len();
            let entry = &mut self.entries[index];
            if entry.length != 0 {
                if entry.length as usize != m.len() || &entry.bytes[..m.len()] != m {
                    return Err("Conflicting duplicate PITCH message".into());
                }
                self.stats.duplicates += 1;
            } else {
                entry.bytes[..m.len()].copy_from_slice(m);
                entry.length = m.len() as u8;
                entry.rx = rx;
                self.buffered += 1;
                self.stats.buffered_high_watermark =
                    self.stats.buffered_high_watermark.max(self.buffered as u64);
                if replay {
                    self.stats.replay_messages += 1;
                }
            }
            offset += m.len();
        }
        self.drain(p, now)
    }
    pub fn tick(&mut self, p: &mut Processor, now: u64) -> Result<(), Error> {
        if now - self.last_tick < 100_000 {
            return Ok(());
        }
        self.last_tick = now;
        if let Some(mut grp) = self.grp.take() {
            let result = grp.tick(now, |m| self.control(p, m, false, now));
            self.grp = Some(grp);
            result?;
        }
        if let Some(mut spin) = self.spin.take() {
            let result = spin.tick(now, |m| self.control(p, m, true, now));
            self.spin = Some(spin);
            result?;
        }
        if self.pending && self.ack && distance(self.request_end, self.expected) <= u32::MAX / 2 {
            self.pending = false;
        }
        if self.pending && now >= self.deadline {
            self.stats.timeouts += 1;
            self.pending = false;
            if !self.spinning {
                self.start_spin(p, now)?;
            }
        }
        if self.spinning {
            if now >= self.spin_deadline {
                return Err("Spin recovery timeout".into());
            }
            let spin = self.spin.as_mut().unwrap();
            if !self.spin_requested
                && spin.logged_in
                && spin.writable()
                && self.image != 0
                && distance(self.expected, next(self.image, 1)) <= u32::MAX / 2
            {
                let mut request = [6, 0x81, 0, 0, 0, 0];
                put(&mut request, 2, self.image as u64, 4);
                spin.send_message(&request)?;
                self.spin_requested = true;
            }
            return Ok(());
        }
        let grp = self.grp.as_mut().unwrap();
        if !self.active
            || self.expected == self.frontier
            || self.pending
            || !grp.logged_in
            || !grp.writable()
        {
            return Ok(());
        }
        if self.stats.requests >= 100000 {
            return self.start_spin(p, now);
        }
        let n = self.stats.requests as usize;
        if (n >= 320 && now - self.request_times[(n - 320) % 1500] < 1_000_000_000)
            || (n >= 1500 && now - self.request_times[n % 1500] < 60_000_000_000)
        {
            return Ok(());
        }
        let available = 100usize
            .min(distance(self.expected, self.frontier) as usize)
            .min(self.entries.len());
        let mut count = 0;
        while count < available
            && self.entries[(self.head + count) % self.entries.len()].length == 0
        {
            count += 1;
        }
        if count == 0 {
            return Ok(());
        }
        let mut request = [9, 3, self.unit, 0, 0, 0, 0, 0, 0];
        put(&mut request, 3, self.expected as u64, 4);
        put(&mut request, 7, count as u64, 2);
        grp.send_message(&request)?;
        self.pending = true;
        self.ack = false;
        self.request_first = self.expected;
        self.request_count = count as u16;
        self.request_end = next(self.expected, count as u32);
        self.deadline = now + self.options.timeout_ns;
        self.request_times[n % 1500] = now;
        self.stats.requests += 1;
        Ok(())
    }
}
