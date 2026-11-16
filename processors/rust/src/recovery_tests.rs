use crate::*;
use std::{cell::RefCell, rc::Rc};

fn message(kind: u8, id: u64, quantity: u32) -> Vec<u8> {
    let size = match kind {
        0x97 => 6,
        0x21 => 34,
        0x23 => 26,
        0x25 => 18,
        _ => unreachable!(),
    };
    let mut b = vec![0; size];
    b[0] = size as u8;
    b[1] = kind;
    if kind != 0x97 {
        b[6..14].copy_from_slice(&id.to_le_bytes());
    }
    if kind == 0x21 {
        b[14] = b'B';
        b[15..19].copy_from_slice(&quantity.to_le_bytes());
        b[19..25].copy_from_slice(b"TEST  ");
        b[25..33].copy_from_slice(&100000u64.to_le_bytes());
        b[33] = 1;
    } else if kind == 0x23 || kind == 0x25 {
        b[14..18].copy_from_slice(&quantity.to_le_bytes());
        if kind == 0x23 {
            b[18..26].copy_from_slice(&123u64.to_le_bytes());
        }
    }
    b
}
fn packet(sequence: u32, messages: &[Vec<u8>]) -> Vec<u8> {
    let mut b = vec![0; 8];
    b[2] = messages.len() as u8;
    b[3] = 1;
    b[4..8].copy_from_slice(&sequence.to_le_bytes());
    for m in messages {
        b.extend_from_slice(m);
    }
    let size = b.len() as u16;
    b[..2].copy_from_slice(&size.to_le_bytes());
    b
}
#[test]
fn held_prefix_preserves_history_and_excludes_catch_up_samples() {
    let mut p = Processor::new(1, 0, 10, LossPolicy::Stop, || 1000, 16, 10).unwrap();
    p.process(
        &packet(
            1,
            &[
                message(0x97, 0, 0),
                message(0x21, 1, 100),
                message(0x23, 1, 10),
            ],
        ),
        0,
    )
    .unwrap();
    assert_eq!(p.latency().samples, 2);
    p.begin_recovery();
    assert!(!p.book_current());
    assert!(p.l1.trade_history_current());
    p.process_sampled(&packet(4, &[message(0x25, 1, 10)]), 0, false)
        .unwrap();
    assert_eq!(p.latency().samples, 2);
    p.finish_recovery();
    assert!(p.book_current());
    assert!(p.loss_seen());
    assert!(p.l1.trade_history_current());
    assert_eq!(p.book.snapshot()[0].quantity, 80);
}
#[test]
fn spin_clears_prior_last_and_publishes_invalidation() {
    let mut p = Processor::new(1, 0, 10, LossPolicy::Stop, || 1000, 16, 10).unwrap();
    p.process(
        &packet(
            1,
            &[
                message(0x97, 0, 0),
                message(0x21, 1, 100),
                message(0x23, 1, 10),
            ],
        ),
        0,
    )
    .unwrap();
    let events = Rc::new(RefCell::new(Vec::new()));
    let observed = Rc::clone(&events);
    p.set_l1_sink(move |event| observed.borrow_mut().push(*event));
    p.begin_spin().unwrap();
    assert!(events.borrow().iter().any(|e| e.changed == 63
        && e.bid.is_none()
        && e.last.is_none()
        && !e.trade_history_current));
    assert_eq!(p.l1.trade_count(), 0);
    assert!(!p.book_current());
    p.spin_message(&message(0x21, 2, 50)).unwrap();
    assert!(!p.l1.trade_history_current());
    p.finish_spin(u32::MAX);
    p.process_sampled(&packet(1, &[message(0x25, 2, 5)]), 0, false)
        .unwrap();
    p.finish_recovery();
    assert_eq!(p.book.snapshot()[0].quantity, 45);
    assert!(p.book_current());
    assert_eq!(p.latency().samples, 2);
    assert!(p.l1.symbols.values().all(|s| s.l1.last.is_none()));
    assert!(p.spin_message(&message(0x23, 2, 1)).is_err());
}
