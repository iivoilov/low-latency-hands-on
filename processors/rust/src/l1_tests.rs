use super::*;
use std::{
    cell::RefCell,
    rc::Rc,
    sync::atomic::{AtomicU64, Ordering},
};
fn put(b: &mut [u8], o: usize, v: u64, w: usize) {
    for i in 0..w {
        b[o + i] = (v >> (8 * i)) as u8;
    }
}
fn packet(sequence: u32, m: &[u8]) -> Vec<u8> {
    let mut b = vec![0; 8];
    let n = 8 + m.len();
    put(&mut b, 0, n as u64, 2);
    b[2] = 1;
    b[3] = 1;
    put(&mut b, 4, sequence as u64, 4);
    b.extend(m);
    b
}
fn fixtures() -> BTreeMap<String, Vec<u8>> {
    let mut result = BTreeMap::new();
    for line in include_str!("../../tests/equities_messages.tsv")
        .lines()
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
    {
        let cols: Vec<_> = line.split_whitespace().collect();
        let b: Vec<u8> = (0..cols[1].len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&cols[1][i..i + 2], 16).unwrap())
            .collect();
        let m = pitch::decode(&b).unwrap();
        assert_eq!(
            std::str::from_utf8(&m.symbol).unwrap().trim_end(),
            if cols[2] == "-" { "" } else { cols[2] }
        );
        let values: Vec<u64> = cols[3..].iter().map(|c| c.parse().unwrap()).collect();
        assert_eq!(
            vec![
                m.price,
                m.quantity as u64,
                m.order_id,
                m.execution_id,
                m.remaining_quantity as u64
            ],
            values
        );
        for n in 2..b.len() {
            if cols[0] == "time_epoch" && n == 6 {
                continue;
            }
            let mut truncated = b[..n].to_vec();
            truncated[0] = n as u8;
            assert!(pitch::decode(&truncated).is_err(), "{} {}", cols[0], n);
        }
        if cols[0] != "time" {
            let mut extended = b.clone();
            extended.extend([0xaa, 0xbb]);
            extended[0] = extended.len() as u8;
            let e = pitch::decode(&extended).unwrap();
            assert_eq!(
                (e.symbol, e.price, e.quantity),
                (m.symbol, m.price, m.quantity)
            );
        }
        result.insert(cols[0].into(), b);
    }
    assert_eq!(result.len(), 23);
    result
}
static L1_TICKS: AtomicU64 = AtomicU64::new(0);
fn tick() -> u64 {
    L1_TICKS.fetch_add(1, Ordering::Relaxed) + 1
}
#[test]
fn all_equities_layouts_and_extensions() {
    let f = fixtures();
    let expanded = pitch::decode(&f["add_expanded"]).unwrap();
    assert_eq!((expanded.participant, expanded.flags), (*b"ABCD", 1));
    let a = pitch::decode(&f["auction_update"]).unwrap();
    assert_eq!(
        (
            a.buy_quantity,
            a.sell_quantity,
            a.indicative_price,
            a.auction_only_price
        ),
        (100, 200, 102600, 102700)
    );
    let t = pitch::decode(&f["time_reference"]).unwrap();
    assert_eq!(
        (t.midnight_reference, t.seconds, t.time_offset, t.trade_date),
        (1699966800, 34200, 123456789, 20261114)
    );
    let mut b = OrderBook::new(2).unwrap();
    b.apply(&f["add_short"]).unwrap();
    b.apply(&f["reduce_short"]).unwrap();
    assert_eq!(b.snapshot()[0].quantity, 80);
    b.apply(&f["modify_short"]).unwrap();
    assert_eq!(
        (b.snapshot()[0].quantity, b.snapshot()[0].price_1e4),
        (90, 102800)
    );
    b.apply(&f["execute"]).unwrap();
    assert_eq!(b.snapshot()[0].quantity, 70);
    assert!(b.apply(&f["add_expanded"]).is_err());
    b.clear();
    b.apply(&f["add_expanded"]).unwrap();
    assert_eq!(b.snapshot()[0].symbol, "LONGSYMB");
}
#[test]
fn same_price_quantities_preserve_levels_and_execution_semantics() {
    fn level(p: &Processor, side: u8, price: u64, quantity: u64, orders: u32) {
        assert_eq!(
            p.book.snapshot(),
            vec![BookLevel {
                symbol: "TEST".into(),
                side: side as char,
                price_1e4: price,
                quantity,
                orders,
            }]
        );
        let l1 = p.l1.symbols.values().next().unwrap().l1;
        let quote = if side == b'B' { l1.bid } else { l1.ask }.unwrap();
        assert_eq!((quote.price, quote.quantity), (price, quantity));
    }
    fn modify(f: &BTreeMap<String, Vec<u8>>, name: &str, quantity: u32, price: u64) -> Vec<u8> {
        let mut m = f[name].clone();
        let short = m[1] == 0x28;
        put(&mut m, 14, quantity as u64, if short { 2 } else { 4 });
        put(
            &mut m,
            if short { 16 } else { 18 },
            if short { price / 100 } else { price },
            if short { 2 } else { 8 },
        );
        m
    }
    let f = fixtures();
    for side in [b'B', b'S'] {
        let mut p =
            Processor::with_trade_limit(1, 0, 100, LossPolicy::Stop, || 0, 8, 4, 8).unwrap();
        let mut sequence = 1;
        let mut send = |p: &mut Processor, m: Vec<u8>| {
            p.process(&packet(sequence, &m), 0).unwrap();
            sequence += 1;
        };
        send(&mut p, f["unit_clear"].clone());
        let mut add = f["add_long"].clone();
        add[14] = side;
        send(&mut p, add.clone());
        put(&mut add, 6, 12, 8);
        send(&mut p, add.clone());
        level(&p, side, 102500, 200, 2);
        send(&mut p, modify(&f, "modify_long", u32::MAX, 102500));
        level(&p, side, 102500, u32::MAX as u64 + 100, 2);
        send(&mut p, modify(&f, "modify_long", 50, 102500));
        level(&p, side, 102500, 150, 2);
        let before = p.stats.l1_updates;
        send(&mut p, modify(&f, "modify_long", 50, 102500));
        assert_eq!(p.stats.l1_updates, before);
        send(&mut p, modify(&f, "modify_short", 65535, 102500));
        level(&p, side, 102500, 65635, 2);
        send(&mut p, modify(&f, "modify_short", 7, 102500));
        level(&p, side, 102500, 107, 2);
        let mut execution = f["execute_price"].clone();
        put(&mut execution, 18, 9, 4);
        send(&mut p, execution.clone());
        level(&p, side, 102500, 109, 2);
        let last = p.l1.symbols.values().next().unwrap().l1.last.unwrap();
        assert_eq!((last.price, last.quantity), (102700, 200));
        put(&mut execution, 22, 113, 8);
        put(&mut execution, 18, 11, 4);
        send(&mut p, execution.clone());
        level(&p, side, 102500, 111, 2);
        put(&mut execution, 22, 114, 8);
        let before = p.stats.l1_updates;
        send(&mut p, execution.clone());
        assert_eq!(p.stats.l1_updates, before + 1);
        assert_eq!(
            p.l1.symbols
                .values()
                .next()
                .unwrap()
                .l1
                .last
                .unwrap()
                .execution_id,
            114
        );
        put(&mut execution, 22, 115, 8);
        put(&mut execution, 18, 0, 4);
        send(&mut p, execution);
        level(&p, side, 102500, 100, 1);
        let mut remove = f["delete"].clone();
        put(&mut remove, 6, 12, 8);
        send(&mut p, remove);
        assert_eq!((p.book.order_count(), p.book.symbol_count()), (0, 0));
        put(&mut add, 6, 11, 8);
        send(&mut p, add);
        send(&mut p, modify(&f, "modify_long", 25, 102500));
        level(&p, side, 102500, 25, 1);
        send(&mut p, modify(&f, "modify_short", 30, 102600));
        level(&p, side, 102600, 30, 1);
        send(&mut p, modify(&f, "modify_long", 0, 102600));
        assert!(p.book.snapshot().is_empty());
        assert_eq!(p.book.symbol_count(), 0);
        let l1 = p.l1.symbols.values().next().unwrap().l1;
        assert!(l1.bid.is_none() && l1.ask.is_none());
        assert_eq!(l1.last.unwrap().execution_id, 115);
        assert!(p.book_current() && !p.loss_seen());
    }
}
#[test]
fn l1_decisions_trades_corrections_and_timing() {
    L1_TICKS.store(0, Ordering::Relaxed);
    let f = fixtures();
    let mut p =
        Processor::with_trade_limit(1, 0, 100, LossPolicy::Stop, tick, 8, 100, 100).unwrap();
    let events = Rc::new(RefCell::new(Vec::new()));
    let output = events.clone();
    p.set_l1_sink(move |e| {
        output
            .borrow_mut()
            .push((*e, L1_TICKS.load(Ordering::Relaxed)));
    });
    let mut sequence = 1;
    let mut send = |p: &mut Processor, m: Vec<u8>| {
        p.process(&packet(sequence, &m), 0).unwrap();
        sequence += 1;
    };
    send(&mut p, f["unit_clear"].clone());
    send(&mut p, f["add_long"].clone());
    assert_eq!(events.borrow().len(), 1);
    let e = events.borrow()[0];
    assert!(e.0.bid.is_some() && e.0.ask.is_none());
    assert_eq!((e.0.changed, e.1), (1, 1));
    let mut deeper = f["add_long"].clone();
    put(&mut deeper, 6, 12, 8);
    put(&mut deeper, 25, 102400, 8);
    send(&mut p, deeper);
    assert_eq!(
        (
            events.borrow().len(),
            p.stats.l1_no_updates,
            L1_TICKS.load(Ordering::Relaxed)
        ),
        (1, 1, 2)
    );
    let mut ask = f["add_short"].clone();
    put(&mut ask, 6, 13, 8);
    ask[14] = b'S';
    put(&mut ask, 23, 1030, 2);
    send(&mut p, ask);
    let e = events.borrow().last().unwrap().0;
    assert_eq!(e.changed, 2);
    assert!(e.bid.is_some());
    assert_eq!(e.ask.unwrap().price, 103000);
    send(&mut p, f["execute_price"].clone());
    let e = events.borrow().last().unwrap().0;
    assert_eq!(e.last.unwrap().price, 102700);
    assert_eq!(e.last.unwrap().quantity, 200);
    assert_eq!(e.bid.unwrap().quantity, 7);
    let mut trade = f["trade_long"].clone();
    put(&mut trade, 33, 113, 8);
    send(&mut p, trade.clone());
    assert_eq!(events.borrow().last().unwrap().0.changed, 4);
    assert_eq!(p.book.snapshot()[0].quantity, 7);
    put(&mut trade, 33, 114, 8);
    send(&mut p, trade);
    assert_eq!(events.borrow().last().unwrap().0.changed, 4);
    let mut broken = f["trade_break"].clone();
    put(&mut broken, 6, 113, 8);
    let before = events.borrow().len();
    send(&mut p, broken.clone());
    assert_eq!(events.borrow().len(), before);
    put(&mut broken, 6, 114, 8);
    send(&mut p, broken);
    assert_eq!(
        events.borrow().last().unwrap().0.last.unwrap().execution_id,
        112
    );
    send(&mut p, f["status"].clone());
    send(&mut p, f["retail"].clone());
    let e = events.borrow().last().unwrap().0;
    assert_eq!((e.trading_status, e.reg_sho, e.retail), (b'T', b'1', b'A'));
    let before = events.borrow().len();
    send(&mut p, f["auction_update"].clone());
    send(&mut p, f["auction_summary"].clone());
    assert_eq!(events.borrow().len(), before);
    send(&mut p, f["unit_clear"].clone());
    let e = events.borrow().last().unwrap().0;
    assert!(e.bid.is_none() && e.ask.is_none());
    assert_eq!((e.last.unwrap().execution_id, e.changed), (112, 3));
    p.note_loss();
    let e = events.borrow().last().unwrap().0;
    assert!(!e.book_current && !e.trade_history_current);
    assert_eq!(e.changed, 32);
    send(&mut p, f["unit_clear"].clone());
    let e = events.borrow().last().unwrap().0;
    assert!(e.book_current && !e.trade_history_current);
    send(&mut p, f["end_session"].clone());
    assert!(p.session_ended);
    assert!(p.process(&packet(sequence, &f["add_long"]), 0).is_err());
}
#[test]
fn bounded_trade_history_and_unknown_breaks() {
    let f = fixtures();
    let mut p =
        Processor::with_trade_limit(1, 0, 100, LossPolicy::Stop, monotonic_ns, 8, 2, 1).unwrap();
    p.process(&packet(1, &f["unit_clear"]), 0).unwrap();
    p.process(&packet(2, &f["trade_long"]), 0).unwrap();
    let mut trade = f["trade_long"].clone();
    put(&mut trade, 33, 112, 8);
    assert!(p.process(&packet(3, &trade), 0).is_err());
    let mut p = Processor::with_trade_limit(1, 0, 100, LossPolicy::Continue, monotonic_ns, 8, 2, 1)
        .unwrap();
    p.process(&packet(1, &f["unit_clear"]), 0).unwrap();
    p.note_loss();
    p.process(&packet(2, &f["trade_break"]), 0).unwrap();
    assert_eq!(p.stats.l1_no_updates, 1);
    let mut p =
        Processor::with_trade_limit(1, 0, 100, LossPolicy::Stop, monotonic_ns, 8, 2, 1).unwrap();
    p.process(&packet(1, &f["unit_clear"]), 0).unwrap();
    assert!(p.process(&packet(2, &f["trade_break"]), 0).is_err());
}

#[test]
fn unsequenced_heartbeats_and_trade_slot_reuse() {
    let f = fixtures();
    let heartbeat = [8, 0, 0, 0, 0, 0, 0, 0];
    let mut p =
        Processor::with_trade_limit(1, 0, 100, LossPolicy::Stop, monotonic_ns, 8, 2, 1).unwrap();
    p.process(&heartbeat, 0).unwrap();
    p.process(&packet(1, &f["unit_clear"]), 0).unwrap();
    p.process(&packet(2, &f["trade_long"]), 0).unwrap();
    p.process(&packet(3, &f["trade_break"]), 0).unwrap();
    assert_eq!(p.l1.trade_count(), 0);
    assert!(p.l1.symbols.is_empty());
    let mut t = f["trade_long"].clone();
    put(&mut t, 33, 112, 8);
    p.process(&packet(4, &t), 0).unwrap();
    p.process(&packet(5, &f["end_session"]), 0).unwrap();
    p.process(&heartbeat, 0).unwrap();
    assert_eq!(
        (p.stats.unsequenced_heartbeats, p.stats.sequence_errors),
        (2, 0)
    );
    let mut p = Processor::with_trade_limit(1, 0, 100, LossPolicy::Continue, monotonic_ns, 8, 2, 1)
        .unwrap();
    p.process(&heartbeat, 0).unwrap();
    p.process(&packet(100, &f["trade_long"]), 0).unwrap();
    assert!(p.stats.joined_midstream);
    assert_eq!(p.stats.missing_messages, 0);
    assert!(!p.l1.trade_history_current());
    let mut p = Processor::with_trade_limit(1, 0, 100, LossPolicy::Continue, monotonic_ns, 8, 1, 1)
        .unwrap();
    p.process(&packet(1, &f["status"]), 0).unwrap();
    let mut status = f["status"].clone();
    status[6] = b'X';
    p.note_loss();
    assert!(p.process(&packet(2, &status), 0).is_err());
}

#[test]
fn execution_ids_are_scoped_to_trade_date() {
    let f = fixtures();
    let mut p =
        Processor::with_trade_limit(1, 0, 100, LossPolicy::Stop, monotonic_ns, 8, 2, 2).unwrap();
    p.process(&packet(1, &f["unit_clear"]), 0).unwrap();
    p.process(&packet(2, &f["time_reference"]), 0).unwrap();
    p.process(&packet(3, &f["trade_long"]), 0).unwrap();
    let mut next = f["time_reference"].clone();
    put(&mut next, 14, 20261115, 4);
    p.process(&packet(4, &next), 0).unwrap();
    p.process(&packet(5, &f["trade_long"]), 0).unwrap();
    assert_eq!(p.l1.trade_count(), 2);
    assert_eq!(
        p.l1.symbols
            .values()
            .next()
            .unwrap()
            .l1
            .last
            .unwrap()
            .trade_date,
        20261115
    );
    p.process(&packet(6, &f["trade_break"]), 0).unwrap();
    let last = p.l1.symbols.values().next().unwrap().l1.last.unwrap();
    assert_eq!((last.trade_date, last.execution_id), (20261114, 111));
}
