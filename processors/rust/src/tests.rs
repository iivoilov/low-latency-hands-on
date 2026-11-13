use super::*;
use std::sync::atomic::{AtomicU64, Ordering};
fn put(bytes: &mut [u8], offset: usize, value: u64, width: usize) {
    for i in 0..width {
        bytes[offset + i] = (value >> (8 * i)) as u8;
    }
}
fn control(kind: u8) -> Vec<u8> {
    vec![6, kind, 0, 0, 0, 0]
}
fn event(kind: u8, id: u64, quantity: u32, price: u64, side: u8) -> Vec<u8> {
    let n = match kind {
        0x21 => 34,
        0x27 => 27,
        0x23 => 26,
        0x25 => 18,
        _ => 14,
    };
    let mut b = vec![0; n];
    b[0] = n as u8;
    b[1] = kind;
    put(&mut b, 6, id, 8);
    match kind {
        0x21 => {
            b[14] = side;
            put(&mut b, 15, quantity as u64, 4);
            b[19..25].copy_from_slice(b"TEST  ");
            put(&mut b, 25, price, 8);
            b[33] = 1;
        }
        0x27 => {
            put(&mut b, 14, quantity as u64, 4);
            put(&mut b, 18, price, 8);
            b[26] = 1;
        }
        0x23 | 0x25 => put(&mut b, 14, quantity as u64, 4),
        _ => (),
    }
    b
}
fn add(id: u64) -> Vec<u8> {
    event(0x21, id, 100, 100000, b'B')
}
fn datagram(sequence: u32, messages: Vec<Vec<u8>>) -> Vec<u8> {
    let mut b = vec![0; 8];
    b[2] = messages.len() as u8;
    b[3] = 1;
    put(&mut b, 4, sequence as u64, 4);
    for m in messages {
        b.extend(m);
    }
    let len = b.len();
    put(&mut b, 0, len as u64, 2);
    b
}
fn zero() -> u64 {
    0
}
fn engine(policy: LossPolicy, slots: usize) -> Processor {
    Processor::new(1, 0, 100, policy, zero, slots).unwrap()
}
#[test]
fn l2_transitions_and_invalid_updates() {
    let mut b = OrderBook::default();
    b.apply(&event(0x21, 1, 100, 100000, b'B')).unwrap();
    b.apply(&event(0x21, 2, 50, 100000, b'B')).unwrap();
    b.apply(&event(0x21, 3, 40, 100200, b'S')).unwrap();
    assert_eq!(b.snapshot()[0].quantity, 150);
    assert_eq!(b.snapshot()[0].orders, 2);
    b.apply(&event(0x27, 2, 70, 99900, b'B')).unwrap();
    assert_eq!(b.snapshot()[1].price_1e4, 99900);
    b.apply(&event(0x25, 1, 30, 0, b'B')).unwrap();
    assert_eq!(b.snapshot()[0].quantity, 70);
    assert!(b.apply(&event(0x23, 1, 71, 0, b'B')).is_err());
    assert_eq!(b.snapshot()[0].quantity, 70);
    b.apply(&event(0x23, 1, 70, 0, b'B')).unwrap();
    b.apply(&event(0x29, 2, 0, 0, b'B')).unwrap();
    b.apply(&event(0x27, 3, 0, 0, b'S')).unwrap();
    assert!(b.snapshot().is_empty());
    assert!(b.apply(&event(0x29, 99, 0, 0, b'B')).is_err());
    assert!(b.apply(&event(0x21, 1, 0, 0, b'B')).is_err());
    assert!(b.apply(&event(0x21, 1, 1, 0, b'X')).is_err());
    b.apply(&add(1)).unwrap();
    assert!(b.apply(&add(1)).is_err());
    let mut extended = add(5);
    extended.extend([0xaa, 0xbb]);
    extended[0] = 36;
    b.apply(&extended).unwrap();
    assert_eq!(b.order_count(), 2);
    b.clear();
    assert!(b.snapshot().is_empty());
    let mut bad = add(8);
    bad[19] = 0;
    assert!(b.apply(&bad).is_err());
    bad[19..25].copy_from_slice(b"      ");
    assert!(b.apply(&bad).is_err());
}
static TICKS: AtomicU64 = AtomicU64::new(0);
fn tick() -> u64 {
    TICKS.fetch_add(1, Ordering::Relaxed) + 1
}
#[test]
fn timing_warmup_exact_percentiles_and_bounded_storage() {
    TICKS.store(0, Ordering::Relaxed);
    let mut p = Processor::new(1, 1, 1000, LossPolicy::Stop, tick, 64).unwrap();
    p.process(&datagram(1, vec![control(0x20), control(0x97), add(1)]), 0)
        .unwrap();
    assert_eq!(TICKS.load(Ordering::Relaxed), 0);
    let mut sequence = 4;
    for packet in 0..25 {
        p.process(
            &datagram(
                sequence,
                (0..40).map(|i| add(2 + packet * 40 + i)).collect(),
            ),
            0,
        )
        .unwrap();
        sequence += 40;
    }
    let l = p.latency();
    assert_eq!(l.samples, 1000);
    assert_eq!(l.min_ns, Some(1));
    assert_eq!(l.p50_ns, Some(500));
    assert_eq!(l.p99_ns, Some(990));
    assert_eq!(l.p99_9_ns, Some(999));
    assert_eq!(l.max_ns, Some(1000));
    assert!(p.complete() && p.book_current());
    p.process(&datagram(sequence, vec![add(2000)]), 0).unwrap();
    assert_eq!(TICKS.load(Ordering::Relaxed), 1000);
    assert_eq!(p.stats.warmup_messages, 1);
    let mut other = engine(LossPolicy::Stop, 4);
    let mut packet = datagram(1, vec![add(1)]);
    packet[3] = 2;
    other.process(&packet, 0).unwrap();
    assert_eq!(other.stats.other_unit_packets, 1);
    assert_eq!(other.book.order_count(), 0);
    let mut bad = engine(LossPolicy::Stop, 4);
    assert!(bad
        .process(&datagram(1, vec![control(0x97), add(1)]), 1)
        .is_err());
}
#[test]
fn strict_gap_is_queued_before_stop() {
    let mut p = engine(LossPolicy::Stop, 8);
    let mut q = p.take_gap_consumer().unwrap();
    p.process(&datagram(1, vec![control(0x97), add(1)]), 0)
        .unwrap();
    assert!(p.process(&datagram(4, vec![add(2)]), 0).is_err());
    assert_eq!(p.stats.missing_messages, 1);
    assert!(!p.book_current());
    assert_eq!(p.book.order_count(), 1);
    assert_eq!(
        q.try_pop(),
        Some(GapRange {
            unit: 1,
            first_sequence: 3,
            last_sequence: 3,
            message_count: 1,
            detected_rx_ns: 0
        })
    );
}
#[test]
fn continuing_loss_duplicates_and_unit_clear() {
    let mut p = engine(LossPolicy::Continue, 8);
    let mut q = p.take_gap_consumer().unwrap();
    p.process(&datagram(1, vec![control(0x97), add(1)]), 0)
        .unwrap();
    p.process(&datagram(4, vec![event(0x25, 99, 1, 0, b'B')]), 0)
        .unwrap();
    assert_eq!(p.stats.skipped_market_messages, 1);
    p.process(&datagram(5, vec![event(0x25, 1, 1, 0, b'B')]), 0)
        .unwrap();
    p.process(&datagram(5, vec![event(0x25, 1, 1, 0, b'B')]), 0)
        .unwrap();
    assert_eq!(p.book.snapshot()[0].quantity, 99);
    assert_eq!(p.stats.out_of_order_packets, 1);
    assert!(!p.book_current());
    p.process(
        &datagram(6, vec![control(0x97), event(0x21, 100, 50, 101000, b'S')]),
        0,
    )
    .unwrap();
    assert!(p.book_current() && p.loss_seen());
    assert_eq!(p.book.order_count(), 1);
    p.note_loss();
    p.process(&datagram(8, vec![control(0x97)]), 0).unwrap();
    assert!(p.book_current());
    assert_eq!(q.try_pop().unwrap().first_sequence, 3);
    assert!(q.try_pop().is_none());
}
#[test]
fn gaps_wrap_heartbeat_prefixes_and_notification_overflow() {
    let mut p = engine(LossPolicy::Continue, 1);
    let mut q = p.take_gap_consumer().unwrap();
    p.process(&datagram(1, vec![control(0x97)]), 0).unwrap();
    p.process(&datagram(4, vec![add(1)]), 0).unwrap();
    p.process(&datagram(7, vec![add(2)]), 0).unwrap();
    assert_eq!(
        (p.stats.gap_ranges_enqueued, p.stats.gap_ranges_dropped),
        (1, 1)
    );
    assert_eq!(q.try_pop().unwrap().first_sequence, 2);
    p.process(&datagram(10, vec![add(3)]), 0).unwrap();
    assert_eq!(q.try_pop().unwrap().first_sequence, 8);
    assert_eq!(p.stats.missing_messages, 6);
    let mut wrap = engine(LossPolicy::Continue, 4);
    let mut q = wrap.take_gap_consumer().unwrap();
    wrap.process(&datagram(u32::MAX - 2, vec![control(0x97)]), 0)
        .unwrap();
    wrap.process(&datagram(2, vec![add(1)]), 0).unwrap();
    assert_eq!(
        q.try_pop(),
        Some(GapRange {
            unit: 1,
            first_sequence: u32::MAX - 1,
            last_sequence: 1,
            message_count: 3,
            detected_rx_ns: 0
        })
    );
    for seq in [50, 3000000000] {
        let mut joined = engine(LossPolicy::Continue, 4);
        joined.process(&datagram(seq, vec![add(1)]), 0).unwrap();
        assert!(joined.stats.joined_midstream);
        assert_eq!(joined.stats.missing_messages, 0);
        assert!(!joined.book_current());
    }
    let mut p = engine(LossPolicy::Continue, 4);
    let mut q = p.take_gap_consumer().unwrap();
    p.process(&datagram(1, vec![control(0x97)]), 0).unwrap();
    p.process(&datagram(5, vec![]), 0).unwrap();
    p.process(&datagram(5, vec![]), 0).unwrap();
    assert_eq!(q.try_pop().unwrap().message_count, 3);
    assert!(q.try_pop().is_none());
    assert_eq!(advance_sequence(u32::MAX, 1), 1);
    assert_eq!(advance_sequence(u32::MAX - 1, 3), 2);
}
#[test]
fn malformed_unknown_and_missing_initialization_are_fatal() {
    for bytes in [
        vec![],
        vec![8, 0, 1, 1, 1, 0, 0, 0],
        datagram(1, vec![vec![2, 0xff]]),
        datagram(1, vec![add(1)]),
    ] {
        let mut p = engine(LossPolicy::Stop, 4);
        assert!(p.process(&bytes, 0).is_err());
        assert!(!p.book_current());
        assert_eq!(p.stats.protocol_errors, 1);
        assert!(p.process(&datagram(1, vec![]), 0).is_err());
    }
    let mut p = engine(LossPolicy::Stop, 4);
    assert!(p.process(&datagram(0, vec![]), 0).is_err());
    let mut bytes = datagram(1, vec![control(0x97)]);
    bytes[8] = 1;
    assert!(validate_pitch(&bytes).is_err());
}
#[test]
fn ring_full_reservations_and_wraparound_preserve_packets() {
    assert!(ring::channel::<Received>(3).is_err());
    let (mut tx, mut rx) = ring::channel::<Received>(2).unwrap();
    {
        let mut slot = tx.reserve().unwrap();
        slot.bytes[0] = 99;
        slot.len = 31;
        slot.commit();
    }
    {
        let mut slot = tx.reserve().unwrap();
        slot.len = 200;
        slot.commit();
    }
    assert!(tx.reserve().is_none());
    let a = rx.read().unwrap();
    assert_eq!((a.len, a.bytes[0]), (31, 99));
    drop(a);
    {
        let _cancelled = tx.reserve().unwrap();
    } // Dropping an uncommitted reservation publishes nothing.
    assert_eq!(rx.read().unwrap().len, 200);
    assert!(rx.is_empty());
}
#[test]
fn concurrent_variable_packets_and_gap_handoff() {
    let (mut tx, mut rx) = ring::channel::<Received>(64).unwrap();
    let worker = std::thread::spawn(move || {
        for n in 0u64..100000 {
            loop {
                if let Some(mut s) = tx.reserve() {
                    s.len = 8 + n as usize % 1450;
                    s.rx_ns = n * 10;
                    s.dropped_before = n / 1000;
                    let len = s.len;
                    s.bytes[..8].copy_from_slice(&n.to_le_bytes());
                    s.bytes[8..len].fill(n as u8);
                    s.commit();
                    break;
                }
                std::thread::yield_now();
            }
        }
    });
    for n in 0u64..100000 {
        loop {
            if let Some(s) = rx.read() {
                assert_eq!(le(&s.bytes[..8]), n);
                assert_eq!(s.len, 8 + n as usize % 1450);
                assert_eq!(s.rx_ns, n * 10);
                assert_eq!(s.dropped_before, n / 1000);
                assert!(s.bytes[8..s.len].iter().all(|b| *b == n as u8));
                break;
            }
            std::thread::yield_now();
        }
    }
    worker.join().unwrap();
    let (mut tx, mut rx) = ring::channel::<GapRange>(64).unwrap();
    let worker = std::thread::spawn(move || {
        for n in 1..=100000 {
            let gap = GapRange {
                unit: 7,
                first_sequence: n,
                last_sequence: n,
                message_count: 1,
                detected_rx_ns: n as u64 * 10,
            };
            while !tx.try_push(gap) {
                std::thread::yield_now();
            }
        }
    });
    for n in 1..=100000 {
        let gap = loop {
            if let Some(g) = rx.try_pop() {
                break g;
            }
            std::thread::yield_now();
        };
        assert_eq!(gap.first_sequence, n);
        assert_eq!(gap.detected_rx_ns, n as u64 * 10);
    }
    worker.join().unwrap();
}
