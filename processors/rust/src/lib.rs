pub mod ring;
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
    pub bytes: [u8; MAX_PAYLOAD],
    pub len: usize,
    pub rx_ns: u64,
    pub dropped_before: u64,
}
impl Default for Received {
    fn default() -> Self {
        Self {
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
    symbol: [u8; 6],
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
type Symbols = BTreeMap<[u8; 6], Sides>;
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
        require(message, 2)?;
        let kind = message[1];
        let minimum = match kind {
            0x21 => 34,
            0x23 => 26,
            0x25 => 18,
            0x27 => 27,
            0x29 => 14,
            _ => return Err("Unsupported PITCH market message".into()),
        };
        require(message, minimum)?; // Validate even when the order is absent.
        if matches!(kind, 0x23 | 0x25) && le(&message[14..18]) == 0 {
            return Err("Invalid zero reduced/executed quantity".into());
        }
        let id = le(&message[6..14]);
        if kind == 0x21 {
            let order = Order {
                symbol: message[19..25].try_into().unwrap(),
                side: message[14],
                quantity: le(&message[15..19]) as u32,
                price: le(&message[25..33]),
            };
            if !matches!(order.side, b'B' | b'S')
                || order.quantity == 0
                || self.orders.contains_key(&id)
            {
                return Err("Invalid/duplicate Add Order".into());
            }
            if order.symbol.iter().any(|b| !(b' '..=b'~').contains(b)) {
                return Err("Invalid PITCH symbol".into());
            }
            if order.symbol.iter().all(|b| *b == b' ') {
                return Err("Empty PITCH symbol".into());
            }
            if self.orders.len() == self.max_orders {
                return Err("PITCH live order limit exceeded".into());
            }
            self.orders.insert(id, order);
            Self::add_level(&mut self.symbols, &order);
            return Ok(());
        }
        let order = self.orders.get_mut(&id).ok_or(BookError::MissingOrder)?;
        match kind {
            0x27 => {
                let quantity = le(&message[14..18]) as u32;
                Self::remove_level(&mut self.symbols, order);
                if quantity == 0 {
                    self.orders.remove(&id);
                } else {
                    order.quantity = quantity;
                    order.price = le(&message[18..26]);
                    Self::add_level(&mut self.symbols, order);
                }
            }
            0x23 | 0x25 => {
                let quantity = le(&message[14..18]) as u32;
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
            _ => return Err("Unsupported PITCH market message".into()),
        }
        Ok(())
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
    pub other_unit_packets: u64,
    pub pitch_messages: u64,
    pub market_messages: u64,
    pub applied_market_messages: u64,
    pub skipped_market_messages: u64,
    pub warmup_messages: u64,
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
    initialized: bool,
    failed: bool,
    dirty: bool,
    loss_seen: bool,
    samples: Vec<u64>,
    pub stats: Stats,
    pub book: OrderBook,
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
            initialized: false,
            failed: false,
            dirty: false,
            loss_seen: false,
            samples: Vec::with_capacity(samples),
            stats: Stats::default(),
            book: OrderBook::new(max_orders)?,
        })
    }
    pub fn take_gap_consumer(&mut self) -> Option<Consumer<GapRange>> {
        self.gap_consumer.take()
    }
    pub fn complete(&self) -> bool {
        self.samples.len() == self.target
    }
    pub fn book_current(&self) -> bool {
        self.initialized && !self.dirty && !self.failed
    }
    pub fn loss_seen(&self) -> bool {
        self.loss_seen
    }
    pub fn note_loss(&mut self) {
        self.dirty = true;
        self.loss_seen = true;
    }
    pub fn process(&mut self, payload: &[u8], rx_ns: u64) -> Result<(), Error> {
        if self.failed {
            return Err("Processor is invalid after a previous protocol error".into());
        }
        let result = self.process_inner(payload, rx_ns);
        if result.is_err() {
            self.failed = true;
            self.stats.protocol_errors += 1;
        }
        result
    }
    fn process_inner(&mut self, payload: &[u8], rx_ns: u64) -> Result<(), Error> {
        validate_pitch(payload)?;
        if payload[3] != self.unit {
            self.stats.other_unit_packets += 1;
            return Ok(());
        }
        self.stats.packets += 1;
        let sequence = le(&payload[4..8]) as u32;
        if sequence != self.expected {
            self.stats.sequence_errors += 1;
            let distance = if sequence >= self.expected {
                sequence as u64 - self.expected as u64
            } else {
                u32::MAX as u64 - self.expected as u64 + sequence as u64
            };
            let joining =
                self.stats.packets == 1 && sequence != 0 && self.policy == LossPolicy::Continue;
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
        let mut offset = 8;
        for _ in 0..payload[2] {
            let message = &payload[offset..offset + payload[offset] as usize];
            match message[1] {
                0x20 => require(message, 6)?,
                0x97 => {
                    require(message, 6)?;
                    self.book.clear();
                    self.initialized = true;
                    self.dirty = false;
                }
                0x21 | 0x23 | 0x25 | 0x27 | 0x29 => {
                    if !self.initialized {
                        if self.policy == LossPolicy::Stop {
                            return Err("Need initial Unit Clear before building L2".into());
                        }
                        self.stats.joined_midstream = true;
                        self.note_loss();
                    }
                    let applied = match self.book.apply_inner(message) {
                        Ok(()) => true,
                        Err(BookError::MissingOrder | BookError::ExcessQuantity)
                            if self.policy == LossPolicy::Continue && self.dirty =>
                        {
                            self.stats.skipped_market_messages += 1;
                            false
                        }
                        Err(error) => return Err(error.message()),
                    };
                    // Endpoint immediately follows the L2 update, before counters/sampling.
                    if applied
                        && self.stats.market_messages >= self.warmup
                        && self.samples.len() < self.target
                    {
                        let end = (self.clock)();
                        if end < rx_ns {
                            return Err("RX/end clocks are inconsistent".into());
                        }
                        self.samples.push(end - rx_ns);
                    }
                    if applied {
                        self.stats.applied_market_messages += 1;
                    }
                    self.stats.market_messages += 1;
                    if self.stats.warmup_messages < self.warmup {
                        self.stats.warmup_messages += 1;
                    }
                }
                kind => return Err(format!("Unsupported PITCH message type {}", kind)),
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
