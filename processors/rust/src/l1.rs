use crate::{
    pitch::{Message, Symbol},
    Error, OrderBook, LARGEST_ORDER_LIMIT,
};
use std::collections::{BTreeMap, HashMap};
pub const DEFAULT_MAX_TRADES: usize = 1_000_000;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Quote {
    pub price: u64,
    pub quantity: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LastTrade {
    pub execution_id: u64,
    pub price: u64,
    pub quantity: u32,
    pub trade_date: u32,
}
#[derive(Default)]
pub struct BookEffect {
    pub symbol: Symbol,
    pub trade: Option<LastTrade>,
}
/// Complete current snapshot. Changed: bid=1, ask=2, last=4, status=8,
/// retail=16, validity=32. Absent sides are explicit, never zero priced.
#[derive(Clone, Copy, Debug)]
pub struct L1Update {
    pub symbol: Symbol,
    pub bid: Option<Quote>,
    pub ask: Option<Quote>,
    pub last: Option<LastTrade>,
    pub trading_status: u8,
    pub reg_sho: u8,
    pub retail: u8,
    pub book_current: bool,
    pub trade_history_current: bool,
    pub changed: u8,
    pub message_type: u8,
}
#[derive(Clone, Debug)]
pub struct SymbolState {
    pub l1: L1Update,
    pub auction_update: Option<Message>,
    pub auction_summary: Option<Message>,
    metadata: bool,
}
#[derive(Clone, Copy)]
struct TradeRecord {
    symbol: Symbol,
    trade: LastTrade,
    previous: Option<(u32, u64)>,
    next: Option<(u32, u64)>,
}
pub struct L1Book {
    pub symbols: BTreeMap<Symbol, SymbolState>,
    trades: HashMap<(u32, u64), TradeRecord>,
    max_symbols: usize,
    max_trades: usize,
    history_current: bool,
    trade_date: u32,
}
impl L1Book {
    pub fn new(max_symbols: usize, max_trades: usize) -> Result<Self, Error> {
        if !(1..=LARGEST_ORDER_LIMIT).contains(&max_trades) {
            return Err("Need 1..100000000 maximum retained trades".into());
        }
        Ok(Self {
            symbols: BTreeMap::new(),
            trades: HashMap::with_capacity(max_trades),
            max_symbols,
            max_trades,
            history_current: true,
            trade_date: 0,
        })
    }
    pub fn trade_count(&self) -> usize {
        self.trades.len()
    }
    pub fn max_trades(&self) -> usize {
        self.max_trades
    }
    pub fn trade_history_current(&self) -> bool {
        self.history_current
    }
    pub fn set_trade_date(&mut self, date: u32) {
        self.trade_date = date;
    }
    pub fn note_loss(&mut self) {
        self.history_current = false;
    }
    fn state(&mut self, symbol: Symbol, current: bool) -> Result<&mut SymbolState, Error> {
        if !self.symbols.contains_key(&symbol) && self.symbols.len() == self.max_symbols {
            return Err("PITCH retained symbol limit exceeded".into());
        }
        Ok(self.symbols.entry(symbol).or_insert(SymbolState {
            l1: L1Update {
                symbol,
                bid: None,
                ask: None,
                last: None,
                trading_status: b'S',
                reg_sho: b'0',
                retail: b'N',
                book_current: current,
                trade_history_current: self.history_current,
                changed: 0,
                message_type: 0,
            },
            auction_update: None,
            auction_summary: None,
            metadata: false,
        }))
    }
    fn update(
        s: &mut SymbolState,
        previous: L1Update,
        book: &OrderBook,
        current: bool,
        history: bool,
        kind: u8,
    ) -> Option<L1Update> {
        let l = &mut s.l1;
        (l.bid, l.ask) = book.top(&l.symbol);
        l.book_current = current;
        l.trade_history_current = history;
        l.message_type = kind;
        l.changed = u8::from(l.bid != previous.bid)
            | (u8::from(l.ask != previous.ask) << 1)
            | (u8::from(l.last != previous.last) << 2)
            | (u8::from(
                l.trading_status != previous.trading_status || l.reg_sho != previous.reg_sho,
            ) << 3)
            | (u8::from(l.retail != previous.retail) << 4)
            | (u8::from(
                l.book_current != previous.book_current
                    || l.trade_history_current != previous.trade_history_current,
            ) << 5);
        if l.changed != 0 {
            Some(*l)
        } else {
            None
        }
    }
    fn empty(s: &SymbolState) -> bool {
        s.l1.bid.is_none() && s.l1.ask.is_none() && s.l1.last.is_none() && !s.metadata
    }
    pub fn decide(
        &mut self,
        m: &Message,
        effect: BookEffect,
        book: &OrderBook,
        current: bool,
    ) -> Result<Option<L1Update>, Error> {
        let kind = m.kind;
        if kind == 0x2c {
            let Some(record) = self.trades.get(&(self.trade_date, m.execution_id)).copied() else {
                return if self.history_current {
                    Err("Trade Break references a missing execution".into())
                } else {
                    Ok(None)
                };
            };
            if let Some(p) = record.previous {
                self.trades.get_mut(&p).unwrap().next = record.next;
            }
            if let Some(n) = record.next {
                self.trades.get_mut(&n).unwrap().previous = record.previous;
            }
            let s = self.symbols.get_mut(&record.symbol).unwrap();
            let previous = s.l1;
            if record.next.is_none() {
                s.l1.last = record.previous.map(|p| self.trades[&p].trade);
            }
            self.trades.remove(&(self.trade_date, m.execution_id));
            let result = Self::update(s, previous, book, current, self.history_current, kind);
            if Self::empty(s) {
                self.symbols.remove(&record.symbol);
            }
            return Ok(result);
        }
        let key = if m.order_update() {
            effect.symbol
        } else {
            m.symbol
        };
        let previous = self.state(key, current)?.l1;
        let trade = if m.trade() {
            Some(LastTrade {
                execution_id: m.execution_id,
                price: m.price,
                quantity: m.quantity,
                trade_date: self.trade_date,
            })
        } else {
            effect.trade
        };
        if let Some(mut trade) = trade {
            trade.trade_date = self.trade_date;
            let key_id = (self.trade_date, trade.execution_id);
            if self.trades.contains_key(&key_id) {
                return Err("Duplicate PITCH execution ID".into());
            }
            if self.trades.len() == self.max_trades {
                return Err("PITCH retained trade limit exceeded".into());
            }
            let prev = previous.last.map(|t| (t.trade_date, t.execution_id));
            self.trades.insert(
                key_id,
                TradeRecord {
                    symbol: key,
                    trade,
                    previous: prev,
                    next: None,
                },
            );
            if let Some(p) = prev {
                self.trades.get_mut(&p).unwrap().next = Some(key_id);
            }
            self.symbols.get_mut(&key).unwrap().l1.last = Some(trade);
        }
        let s = self.symbols.get_mut(&key).unwrap();
        match kind {
            0x31 => {
                s.l1.trading_status = m.indicator;
                s.l1.reg_sho = m.reg_sho;
                s.metadata = true;
            }
            0x98 => {
                s.l1.retail = m.indicator;
                s.metadata = true;
            }
            0x95 => {
                s.auction_update = Some(*m);
                s.metadata = true;
            }
            // Summaries can repeat/reprice and duplicate individual fills.
            // Keep auction metadata; do not double-count it as a last sale.
            0x96 => {
                s.auction_summary = Some(*m);
                s.metadata = true;
            }
            _ => (),
        }
        let result = Self::update(s, previous, book, current, self.history_current, kind);
        if Self::empty(s) {
            self.symbols.remove(&key);
        }
        Ok(result)
    }
    // Unit Clear affects orders only. Trade history/metadata survive it.
    pub fn refresh(&mut self, book: &OrderBook, current: bool, kind: u8) -> Vec<L1Update> {
        let mut events = Vec::new();
        self.symbols.retain(|_, s| {
            if let Some(e) = Self::update(s, s.l1, book, current, self.history_current, kind) {
                events.push(e);
            }
            !Self::empty(s)
        });
        events
    }
}
