pub mod l1;
pub mod pitch;
pub mod ring;
pub use l1::{BookEffect, L1Book, L1Update, LastTrade, Quote};
use pitch::{Message, Symbol};
pub mod recovery;
pub mod socket;
use ring::{Consumer, Producer};
use std::collections::{BTreeMap, HashMap};
pub const MAX_PAYLOAD: usize = 1472;
pub const DEFAULT_MAX_ORDERS: usize = 1_000_000;
pub const LARGEST_ORDER_LIMIT: usize = 100_000_000;
pub type Error = String;

enum BookError {
    Fatal(Error),
    MissingOrder,
    ExcessQuantity,
}
impl From<Error> for BookError {
    fn from(error: Error) -> Self {
        Self::Fatal(error)
    }
}
impl From<&str> for BookError {
    fn from(error: &str) -> Self {
        Self::Fatal(error.into())
    }
}
impl BookError {
    fn message(self) -> Error {
        match self {
            Self::Fatal(error) => error,
            Self::MissingOrder => "PITCH update references a missing order".into(),
            Self::ExcessQuantity => "Reduced/executed quantity exceeds known order".into(),
        }
    }
}

pub fn monotonic_ns() -> u64 {
    let mut ts = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // SAFETY: ts is writable, and CLOCK_MONOTONIC is supported on Linux.
    let status = unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    assert_eq!(
        status,
        0,
        "clock_gettime: {}",
        std::io::Error::last_os_error()
    );
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}
pub fn clock_resolution_ns() -> u64 {
    let mut ts = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // SAFETY: ts is writable and the clock is valid.
    assert_eq!(
        unsafe { libc::clock_getres(libc::CLOCK_MONOTONIC, &mut ts) },
        0
    );
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}
pub fn advance_sequence(sequence: u32, count: u8) -> u32 {
    ((sequence as u64 - 1 + count as u64) % u32::MAX as u64 + 1) as u32
}
fn le(bytes: &[u8]) -> u64 {
    bytes
        .iter()
        .enumerate()
        .fold(0, |v, (i, b)| v | (*b as u64) << (8 * i))
}
fn require(bytes: &[u8], n: usize) -> Result<(), Error> {
    if bytes.len() < n {
        Err("Truncated PITCH message fields".into())
    } else {
        Ok(())
    }
}
pub fn validate_pitch(bytes: &[u8]) -> Result<(), Error> {
    if !(8..=MAX_PAYLOAD).contains(&bytes.len()) {
        return Err("PITCH payload must contain 8..1472 bytes".into());
    }
    if le(&bytes[..2]) as usize != bytes.len() {
        return Err("PITCH header length does not match UDP payload".into());
    }
    let mut offset = 8;
    for _ in 0..bytes[2] {
        if offset + 2 > bytes.len()
            || bytes[offset] < 2
            || offset + bytes[offset] as usize > bytes.len()
        {
            return Err("Truncated or invalid PITCH message".into());
        }
        offset += bytes[offset] as usize;
    }
    if offset != bytes.len() {
        return Err("PITCH message count does not match UDP payload".into());
    }
    Ok(())
}
#[derive(Clone, Copy)]
pub struct Received {
    pub replay: bool,
    pub bytes: [u8; MAX_PAYLOAD],
    pub len: usize,
    pub rx_ns: u64,
    pub dropped_before: u64,
}
impl Default for Received {
    fn default() -> Self {
        Self {
            replay: false,
            bytes: [0; MAX_PAYLOAD],
            len: 0,
            rx_ns: 0,
            dropped_before: 0,
        }
    }
}
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct GapRange {
    pub unit: u8,
    pub first_sequence: u32,
    pub last_sequence: u32,
    pub message_count: u32,
    pub detected_rx_ns: u64,
}
#[derive(Clone, Copy)]
struct Order {
    symbol: Symbol,
    side: u8,
    price: u64,
    quantity: u32,
}
#[derive(Default)]
struct Level {
    quantity: u64,
    orders: u32,
}
type Levels = BTreeMap<u64, Level>;
#[derive(Default)]
struct Sides {
    bids: Levels,
    asks: Levels,
}
type Symbols = BTreeMap<Symbol, Sides>;
#[derive(Debug, PartialEq, Eq)]
pub struct BookLevel {
    pub symbol: String,
    pub side: char,
    pub price_1e4: u64,
    pub quantity: u64,
    pub orders: u32,
}
pub struct OrderBook {
    orders: HashMap<u64, Order>,
    symbols: Symbols,
    max_orders: usize,
}
impl Default for OrderBook {
    fn default() -> Self {
        Self::new(DEFAULT_MAX_ORDERS).expect("Valid default live order limit")
    }
}
impl OrderBook {
    pub fn new(max_orders: usize) -> Result<Self, Error> {
        if !(1..=LARGEST_ORDER_LIMIT).contains(&max_orders) {
            return Err("Need 1..100000000 maximum live orders".into());
        }
        Ok(Self {
            orders: HashMap::with_capacity(max_orders),
            symbols: BTreeMap::new(),
            max_orders,
        })
    }
    pub fn max_orders(&self) -> usize {
        self.max_orders
    }
    pub fn clear(&mut self) {
        self.orders.clear();
        self.symbols.clear();
    }
    pub fn order_count(&self) -> usize {
        self.orders.len()
    }
    pub fn symbol_count(&self) -> usize {
        self.symbols.len()
    }
    fn levels<'a>(symbols: &'a mut Symbols, order: &Order) -> &'a mut Levels {
        let sides = symbols.entry(order.symbol).or_default();
        if order.side == b'B' {
            &mut sides.bids
        } else {
            &mut sides.asks
        }
    }
    fn add_level(symbols: &mut Symbols, order: &Order) {
        let level = Self::levels(symbols, order).entry(order.price).or_default();
        level.quantity += order.quantity as u64;
        level.orders += 1;
    }
    fn remove_level(symbols: &mut Symbols, order: &Order) {
        let side = Self::levels(symbols, order);
        let level = side.get_mut(&order.price).expect("L2 invariant");
        level.quantity -= order.quantity as u64;
        level.orders -= 1;
        if level.orders == 0 {
            side.remove(&order.price);
        }
        let sides = symbols.get(&order.symbol).expect("L2 invariant");
        if sides.bids.is_empty() && sides.asks.is_empty() {
            symbols.remove(&order.symbol);
        }
    }
    pub fn apply(&mut self, message: &[u8]) -> Result<(), Error> {
        self.apply_inner(message).map_err(BookError::message)
    }
    fn apply_inner(&mut self, message: &[u8]) -> Result<(), BookError> {
        self.apply_decoded(&pitch::decode(message)?).map(|_| ())
    }
    fn apply_decoded(&mut self, message: &Message) -> Result<BookEffect, BookError> {
        let kind = message.kind;
        let id = message.order_id;
        if matches!(kind, 0x21 | 0x22 | 0x2f) {
            let order = Order {
                symbol: message.symbol,
                side: message.side,
                quantity: message.quantity,
                price: message.price,
            };
            if self.orders.contains_key(&id) {
                return Err("Invalid/duplicate Add Order".into());
            }
            if self.orders.len() == self.max_orders {
                return Err("PITCH live order limit exceeded".into());
            }
            self.orders.insert(id, order);
            Self::add_level(&mut self.symbols, &order);
            return Ok(BookEffect {
                symbol: order.symbol,
                trade: None,
            });
        }
        if !message.order_update() {
            return Err("Unsupported PITCH order message".into());
        }
        let order = self.orders.get_mut(&id).ok_or(BookError::MissingOrder)?;
        let effect = BookEffect {
            symbol: order.symbol,
            trade: if matches!(kind, 0x23 | 0x24) {
                Some(LastTrade {
                    execution_id: message.execution_id,
                    price: if kind == 0x23 {
                        order.price
                    } else {
                        message.price
                    },
                    quantity: message.quantity,
                    trade_date: 0,
                })
            } else {
                None
            },
        };
        match kind {
            0x27 | 0x28 | 0x24 => {
                // Execution-at-price/size reports authoritative displayed remainder.
                let quantity = if kind == 0x24 {
                    message.remaining_quantity
                } else {
                    message.quantity
                };
                let price = if kind == 0x24 {
                    order.price
                } else {
                    message.price
                };
                if quantity != 0 && price == order.price {
                    let level = Self::levels(&mut self.symbols, order)
                        .get_mut(&order.price)
                        .expect("L2 invariant");
                    level.quantity = level.quantity - order.quantity as u64 + quantity as u64;
                    order.quantity = quantity;
                    return Ok(effect);
                }
                Self::remove_level(&mut self.symbols, order);
                if quantity == 0 {
                    self.orders.remove(&id);
                } else {
                    order.quantity = quantity;
                    order.price = price;
                    Self::add_level(&mut self.symbols, order);
                }
            }
            0x23 | 0x25 | 0x26 => {
                let quantity = message.quantity;
                if quantity > order.quantity {
                    return Err(BookError::ExcessQuantity);
                }
                Self::levels(&mut self.symbols, order)
                    .get_mut(&order.price)
                    .unwrap()
                    .quantity -= quantity as u64;
                order.quantity -= quantity;
                if order.quantity == 0 {
                    Self::remove_level(&mut self.symbols, order);
                    self.orders.remove(&id);
                }
            }
            0x29 => {
                Self::remove_level(&mut self.symbols, order);
                self.orders.remove(&id);
            }
            _ => return Err("Unsupported PITCH order message".into()),
        }
        Ok(effect)
    }
    pub fn top(&self, symbol: &Symbol) -> (Option<Quote>, Option<Quote>) {
        let Some(s) = self.symbols.get(symbol) else {
            return (None, None);
        };
        let bid = s.bids.last_key_value().map(|(p, l)| Quote {
            price: *p,
            quantity: l.quantity,
        });
        let ask = s.asks.first_key_value().map(|(p, l)| Quote {
            price: *p,
            quantity: l.quantity,
        });
        (bid, ask)
    }
    pub fn snapshot(&self) -> Vec<BookLevel> {
        let mut result = Vec::new();
        for (symbol, sides) in &self.symbols {
            let name = std::str::from_utf8(symbol).unwrap().trim_end_matches(' ');
            for (side, levels) in [('B', &sides.bids), ('S', &sides.asks)] {
                let mut append = |price: &u64, level: &Level| {
                    result.push(BookLevel {
                        symbol: name.into(),
                        side,
                        price_1e4: *price,
                        quantity: level.quantity,
                        orders: level.orders,
                    })
                };
                if side == 'B' {
                    for (p, l) in levels.iter().rev() {
                        append(p, l);
                    }
                } else {
                    for (p, l) in levels {
                        append(p, l);
                    }
                }
            }
        }
        result
    }
}
#[derive(Default, Debug)]
pub struct Stats {
    pub joined_midstream: bool,
    pub packets: u64,
    pub unsequenced_heartbeats: u64,
    pub other_unit_packets: u64,
    pub pitch_messages: u64,
    pub market_messages: u64,
    pub applied_market_messages: u64,
    pub skipped_market_messages: u64,
    pub warmup_messages: u64,
    pub l1_decisions: u64,
    pub l1_updates: u64,
    pub l1_no_updates: u64,
    pub sequence_errors: u64,
    pub missing_messages: u64,
    pub out_of_order_packets: u64,
    pub protocol_errors: u64,
    pub gap_ranges_enqueued: u64,
    pub gap_ranges_dropped: u64,
}
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum LossPolicy {
    Stop,
    Continue,
}
#[derive(Debug)]
pub struct Latency {
    pub samples: usize,
    pub min_ns: Option<u64>,
    pub p50_ns: Option<u64>,
    pub p99_ns: Option<u64>,
    pub p99_9_ns: Option<u64>,
    pub max_ns: Option<u64>,
    pub mean_ns: Option<f64>,
}
pub struct Processor {
    unit: u8,
    warmup: u64,
    target: usize,
    clock: fn() -> u64,
    policy: LossPolicy,
    gaps: Producer<GapRange>,
    gap_consumer: Option<Consumer<GapRange>>,
    expected: u32,
    sequenced_seen: bool,
    initialized: bool,
    failed: bool,
    dirty: bool,
    loss_seen: bool,
    recovering: bool,
    samples: Vec<u64>,
    pub stats: Stats,
    pub book: OrderBook,
    pub l1: L1Book,
    sink: Option<Box<dyn FnMut(&L1Update)>>,
    pub time: Message,
    pub time_reference: Message,
    pub session_ended: bool,
}
impl Processor {
    pub fn new(
        unit: u8,
        warmup: u64,
        samples: usize,
        policy: LossPolicy,
        clock: fn() -> u64,
        gap_slots: usize,
        max_orders: usize,
    ) -> Result<Self, Error> {
        Self::with_trade_limit(
            unit,
            warmup,
            samples,
            policy,
            clock,
            gap_slots,
            max_orders,
            l1::DEFAULT_MAX_TRADES,
        )
    }
    #[allow(clippy::too_many_arguments)]
    pub fn with_trade_limit(
        unit: u8,
        warmup: u64,
        samples: usize,
        policy: LossPolicy,
        clock: fn() -> u64,
        gap_slots: usize,
        max_orders: usize,
        max_trades: usize,
    ) -> Result<Self, Error> {
        if unit == 0 || samples == 0 {
            return Err("Need nonzero unit/sample target".into());
        }
        let (gaps, consumer) = ring::channel(gap_slots)?;
        Ok(Self {
            unit,
            warmup,
            target: samples,
            clock,
            policy,
            gaps,
            gap_consumer: Some(consumer),
            expected: 1,
            sequenced_seen: false,
            initialized: false,
            failed: false,
            dirty: false,
            loss_seen: false,
            recovering: false,
            samples: Vec::with_capacity(samples),
            stats: Stats::default(),
            book: OrderBook::new(max_orders)?,
            l1: L1Book::new(max_orders, max_trades)?,
            sink: None,
            time: Message::default(),
            time_reference: Message::default(),
            session_ended: false,
        })
    }
    pub fn take_gap_consumer(&mut self) -> Option<Consumer<GapRange>> {
        self.gap_consumer.take()
    }
    pub fn complete(&self) -> bool {
        self.samples.len() == self.target
    }
    pub fn book_current(&self) -> bool {
        self.initialized && !self.dirty && !self.failed && !self.recovering
    }
    pub fn loss_seen(&self) -> bool {
        self.loss_seen
    }
    pub fn note_loss(&mut self) {
        self.dirty = true;
        self.loss_seen = true;
        self.l1.note_loss();
        for e in self.l1.refresh(&self.book, false, 0) {
            self.emit(e);
        }
    }
    pub fn begin_recovery(&mut self) {
        self.recovering = true;
        self.loss_seen = true;
        for e in self.l1.refresh(&self.book, false, 0) {
            self.emit(e);
        }
    }
    pub fn finish_recovery(&mut self) {
        self.recovering = false;
        for e in self.l1.refresh(&self.book, self.book_current(), 0) {
            self.emit(e);
        }
    }
    pub fn begin_spin(&mut self) -> Result<(), Error> {
        self.begin_recovery();
        let empty: Vec<_> = self
            .l1
            .symbols
            .values()
            .map(|state| {
                let mut e = state.l1;
                e.bid = None;
                e.ask = None;
                e.last = None;
                e.trading_status = b'S';
                e.reg_sho = b'0';
                e.retail = b'N';
                e.book_current = false;
                e.trade_history_current = false;
                e.changed = 63;
                e
            })
            .collect();
        for e in empty {
            self.emit(e);
        }
        self.book.clear();
        self.l1 = L1Book::new(self.book.max_orders(), self.l1.max_trades())?;
        self.l1.note_loss();
        self.time = Message::default();
        self.time_reference = Message::default();
        self.initialized = false;
        self.dirty = false;
        self.session_ended = false;
        Ok(())
    }
    pub fn spin_message(&mut self, bytes: &[u8]) -> Result<(), Error> {
        let m = pitch::decode(bytes)?;
        match m.kind {
            0x20 => self.time = m,
            0xb1 => {
                self.time_reference = m;
                self.l1.set_trade_date(m.trade_date);
            }
            0x21 | 0x22 | 0x2f | 0x31 | 0x95 | 0x98 => {
                let effect = if m.order_update() {
                    self.book.apply_decoded(&m).map_err(BookError::message)?
                } else {
                    BookEffect::default()
                };
                self.l1.decide(&m, effect, &self.book, false)?;
            }
            _ => return Err("Unexpected equities spin message".into()),
        }
        Ok(())
    }
    pub fn finish_spin(&mut self, sequence: u32) {
        self.expected = advance_sequence(sequence, 1);
        self.sequenced_seen = true;
        self.initialized = true;
        let events: Vec<_> = self
            .l1
            .symbols
            .values()
            .map(|s| {
                let mut e = s.l1;
                e.changed = 63;
                e
            })
            .collect();
        for e in events {
            self.emit(e);
        }
    }
    pub fn set_l1_sink(&mut self, sink: impl FnMut(&L1Update) + 'static) {
        self.sink = Some(Box::new(sink));
    }
    fn emit(&mut self, update: L1Update) {
        self.stats.l1_updates += 1;
        if let Some(sink) = &mut self.sink {
            sink(&update);
        }
    }
    pub fn process(&mut self, payload: &[u8], rx_ns: u64) -> Result<(), Error> {
        self.process_sampled(payload, rx_ns, true)
    }
    pub fn process_sampled(
        &mut self,
        payload: &[u8],
        rx_ns: u64,
        sample: bool,
    ) -> Result<(), Error> {
        if self.failed {
            return Err("Processor is invalid after a previous protocol error".into());
        }
        let result = self.process_inner(payload, rx_ns, sample);
        if result.is_err() {
            self.failed = true;
            self.stats.protocol_errors += 1;
        }
        result
    }
    fn process_inner(&mut self, payload: &[u8], rx_ns: u64, sample: bool) -> Result<(), Error> {
        validate_pitch(payload)?;
        let sequence = le(&payload[4..8]) as u32;
        if sequence == 0 && payload[2] == 0 && (payload[3] == self.unit || payload[3] == 0) {
            self.stats.packets += 1;
            self.stats.unsequenced_heartbeats += 1;
            return Ok(());
        }
        if payload[3] != self.unit {
            self.stats.other_unit_packets += 1;
            return Ok(());
        }
        self.stats.packets += 1;
        if sequence != self.expected {
            self.stats.sequence_errors += 1;
            let distance = if sequence >= self.expected {
                sequence as u64 - self.expected as u64
            } else {
                u32::MAX as u64 - self.expected as u64 + sequence as u64
            };
            let joining =
                !self.sequenced_seen && sequence != 0 && self.policy == LossPolicy::Continue;
            if joining {
                self.stats.joined_midstream = true;
                self.note_loss();
            } else if sequence != 0 && distance <= u32::MAX as u64 / 2 {
                self.stats.missing_messages += distance;
                let gap = GapRange {
                    unit: self.unit,
                    first_sequence: self.expected,
                    last_sequence: if sequence == 1 {
                        u32::MAX
                    } else {
                        sequence - 1
                    },
                    message_count: distance as u32,
                    detected_rx_ns: rx_ns,
                };
                if self.gaps.try_push(gap) {
                    self.stats.gap_ranges_enqueued += 1;
                } else {
                    self.stats.gap_ranges_dropped += 1;
                }
                self.note_loss();
            } else {
                self.stats.out_of_order_packets += 1;
            }
            if self.policy == LossPolicy::Stop || sequence == 0 {
                return Err(format!(
                    "PITCH sequence mismatch: expected {}, received {}",
                    self.expected, sequence
                ));
            }
            if !joining && distance > u32::MAX as u64 / 2 {
                return Ok(());
            }
        }
        self.sequenced_seen = true;
        let mut offset = 8;
        for _ in 0..payload[2] {
            let message = &payload[offset..offset + payload[offset] as usize];
            let decoded = pitch::decode(message)?;
            let kind = decoded.kind;
            if self.session_ended {
                return Err("Sequenced message after End of Session".into());
            }
            match kind {
                0x20 => self.time = decoded,
                0xb1 => {
                    self.time_reference = decoded;
                    self.l1.set_trade_date(decoded.trade_date);
                }
                0x2d => self.session_ended = true,
                0x97 => {
                    self.book.clear();
                    self.initialized = true;
                    self.dirty = false;
                    for e in self.l1.refresh(&self.book, self.book_current(), kind) {
                        self.emit(e);
                    }
                }
                _ if decoded.market() => {
                    if !self.initialized && decoded.order_update() {
                        if self.policy == LossPolicy::Stop {
                            return Err("Need initial Unit Clear before building L2".into());
                        }
                        self.stats.joined_midstream = true;
                        self.note_loss();
                    }
                    let effect = if decoded.order_update() {
                        self.book.apply_decoded(&decoded)
                    } else {
                        Ok(BookEffect::default())
                    };
                    let effect = match effect {
                        Ok(effect) => Some(effect),
                        Err(BookError::MissingOrder | BookError::ExcessQuantity)
                            if self.policy == LossPolicy::Continue && self.dirty =>
                        {
                            self.stats.skipped_market_messages += 1;
                            None
                        }
                        Err(e) => return Err(e.message()),
                    };
                    if let Some(effect) = effect {
                        let update =
                            self.l1
                                .decide(&decoded, effect, &self.book, self.book_current())?;
                        // Includes L2, trades and the emit/no-update decision;
                        // synchronous consumer dispatch follows the endpoint.
                        if sample
                            && self.stats.market_messages >= self.warmup
                            && self.samples.len() < self.target
                        {
                            let end = (self.clock)();
                            if end < rx_ns {
                                return Err("RX/end clocks are inconsistent".into());
                            }
                            self.samples.push(end - rx_ns);
                        }
                        self.stats.applied_market_messages += 1;
                        self.stats.l1_decisions += 1;
                        if let Some(update) = update {
                            self.emit(update);
                        } else {
                            self.stats.l1_no_updates += 1;
                        }
                    }
                    self.stats.market_messages += 1;
                    if self.stats.warmup_messages < self.warmup {
                        self.stats.warmup_messages += 1;
                    }
                }
                _ => (),
            }
            self.stats.pitch_messages += 1;
            offset += message.len();
        }
        self.expected = advance_sequence(sequence, payload[2]);
        Ok(())
    }
    pub fn latency(&self) -> Latency {
        let mut result = Latency {
            samples: self.samples.len(),
            min_ns: None,
            p50_ns: None,
            p99_ns: None,
            p99_9_ns: None,
            max_ns: None,
            mean_ns: None,
        };
        if self.samples.is_empty() {
            return result;
        }
        let mut sorted = self.samples.clone();
        sorted.sort_unstable();
        let p = |num: usize, den: usize| Some(sorted[(sorted.len() * num).div_ceil(den) - 1]);
        result.min_ns = sorted.first().copied();
        result.p50_ns = p(50, 100);
        result.p99_ns = p(99, 100);
        result.p99_9_ns = p(999, 1000);
        result.max_ns = sorted.last().copied();
        result.mean_ns = Some(sorted.iter().map(|x| *x as f64).sum::<f64>() / sorted.len() as f64);
        result
    }
}
#[cfg(test)]
mod tests;

#[cfg(test)]
mod l1_tests;

#[cfg(test)]
mod recovery_tests;
