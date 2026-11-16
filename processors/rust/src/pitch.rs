//! Normalized U.S. equities multicast PITCH; prices are in 0.0001 units.
use crate::{le, require, Error};
pub type Symbol = [u8; 8];
#[derive(Clone, Copy, Debug)]
pub struct Message {
    pub kind: u8,
    pub time_offset: u32,
    pub order_id: u64,
    pub execution_id: u64,
    pub price: u64,
    pub quantity: u32,
    pub remaining_quantity: u32,
    pub symbol: Symbol,
    pub side: u8,
    pub indicator: u8,
    pub reg_sho: u8,
    pub auction_type: u8,
    pub flags: u8,
    pub participant: [u8; 4],
    pub customer: u8,
    pub seconds: u32,
    pub epoch_seconds: u32,
    pub midnight_reference: u32,
    pub trade_date: u32,
    pub indicative_price: u64,
    pub auction_only_price: u64,
    pub buy_quantity: u32,
    pub sell_quantity: u32,
}
impl Default for Message {
    fn default() -> Self {
        Self {
            kind: 0,
            time_offset: 0,
            order_id: 0,
            execution_id: 0,
            price: 0,
            quantity: 0,
            remaining_quantity: 0,
            symbol: [b' '; 8],
            side: b' ',
            indicator: b' ',
            reg_sho: b' ',
            auction_type: b' ',
            flags: 0,
            participant: [b' '; 4],
            customer: b' ',
            seconds: 0,
            epoch_seconds: 0,
            midnight_reference: 0,
            trade_date: 0,
            indicative_price: 0,
            auction_only_price: 0,
            buy_quantity: 0,
            sell_quantity: 0,
        }
    }
}
impl Message {
    pub fn order_update(&self) -> bool {
        (0x21..=0x29).contains(&self.kind) || self.kind == 0x2f
    }
    pub fn trade(&self) -> bool {
        matches!(self.kind, 0x2a | 0x2b | 0x30)
    }
    pub fn market(&self) -> bool {
        self.order_update() || self.trade() || matches!(self.kind, 0x2c | 0x31 | 0x95 | 0x96 | 0x98)
    }
}
fn symbol_at(b: &[u8], offset: usize, width: usize) -> Result<Symbol, Error> {
    let mut symbol = [b' '; 8];
    symbol[..width].copy_from_slice(&b[offset..offset + width]);
    if symbol.iter().any(|c| !(b' '..=b'~').contains(c)) {
        return Err("Invalid PITCH symbol".into());
    }
    if symbol.iter().all(|c| *c == b' ') {
        return Err("Empty PITCH symbol".into());
    }
    Ok(symbol)
}
fn choice(c: u8, choices: &[u8]) -> Result<(), Error> {
    if choices.contains(&c) {
        Ok(())
    } else {
        Err("Invalid PITCH indicator".into())
    }
}
pub fn decode(b: &[u8]) -> Result<Message, Error> {
    require(b, 2)?;
    if b[0] as usize != b.len() {
        return Err("Invalid PITCH message length".into());
    }
    let mut m = Message {
        kind: b[1],
        ..Message::default()
    };
    let size = match m.kind {
        0x20 | 0x97 | 0x2d => 6,
        0xb1 | 0x31 | 0x25 => 18,
        0x21 => 34,
        0x22 | 0x23 => 26,
        0x2f | 0x2a => 41,
        0x24 => 38,
        0x26 => 16,
        0x27 | 0x96 => 27,
        0x28 => 19,
        0x29 | 0x2c => 14,
        0x2b => 33,
        0x30 => 43,
        0x95 => 47,
        0x98 => 15,
        _ => {
            return Err(format!(
                "Unsupported equities PITCH message type {}",
                m.kind
            ))
        }
    };
    require(b, size)?;
    let read = |o: usize, w: usize| le(&b[o..o + w]);
    if m.kind == 0x20 {
        m.seconds = read(2, 4) as u32;
        if b.len() > 6 && b.len() < 10 {
            return Err("Truncated PITCH epoch time".into());
        }
        if b.len() >= 10 {
            m.epoch_seconds = read(6, 4) as u32;
        }
        return Ok(m);
    }
    m.time_offset = read(2, 4) as u32;
    if m.kind == 0xb1 {
        m.midnight_reference = m.time_offset;
        m.seconds = read(6, 4) as u32;
        m.time_offset = read(10, 4) as u32;
        m.trade_date = read(14, 4) as u32;
    } else if matches!(m.kind, 0x21 | 0x22 | 0x2f) || m.trade() {
        let short = matches!(m.kind, 0x22 | 0x2b);
        let expanded = matches!(m.kind, 0x2f | 0x30);
        let so = if short { 17 } else { 19 };
        let po = if short {
            23
        } else if expanded {
            27
        } else {
            25
        };
        m.order_id = read(6, 8);
        m.side = b[14];
        choice(m.side, if m.trade() { b"B" } else { b"BS" })?;
        m.quantity = read(15, if short { 2 } else { 4 }) as u32;
        m.symbol = symbol_at(b, so, if expanded { 8 } else { 6 })?;
        m.price = read(po, if short { 2 } else { 8 }) * if short { 100 } else { 1 };
        if m.trade() {
            m.execution_id = read(po + if short { 2 } else { 8 }, 8);
        } else {
            m.flags = b[po + if short { 2 } else { 8 }];
            if expanded {
                m.participant.copy_from_slice(&b[36..40]);
                m.customer = b[40];
            }
        }
        if m.quantity == 0 {
            return Err("Invalid zero added/traded quantity".into());
        }
    } else if (0x23..=0x29).contains(&m.kind) {
        m.order_id = read(6, 8);
        if m.kind != 0x29 {
            m.quantity = read(14, if matches!(m.kind, 0x26 | 0x28) { 2 } else { 4 }) as u32;
            if !matches!(m.kind, 0x27 | 0x28) && m.quantity == 0 {
                return Err("Invalid zero reduced/executed quantity".into());
            }
        }
        if matches!(m.kind, 0x23 | 0x24) {
            m.execution_id = read(if m.kind == 0x23 { 18 } else { 22 }, 8);
            if m.kind == 0x24 {
                m.remaining_quantity = read(18, 4) as u32;
                m.price = read(30, 8);
            }
        } else if matches!(m.kind, 0x27 | 0x28) {
            m.price = read(
                if m.kind == 0x27 { 18 } else { 16 },
                if m.kind == 0x27 { 8 } else { 2 },
            ) * if m.kind == 0x27 { 1 } else { 100 };
            m.flags = b[if m.kind == 0x27 { 26 } else { 18 }];
        }
    } else if m.kind == 0x2c {
        m.execution_id = read(6, 8);
    } else if matches!(m.kind, 0x31 | 0x95 | 0x96 | 0x98) {
        m.symbol = symbol_at(b, 6, 8)?;
        if m.kind == 0x31 {
            m.indicator = b[14];
            choice(m.indicator, b"AEHLOPQST")?;
            m.reg_sho = b[15];
            choice(m.reg_sho, b"01")?;
        } else if m.kind == 0x98 {
            m.indicator = b[14];
            choice(m.indicator, b"BSAN")?;
        } else {
            m.auction_type = b[14];
            choice(m.auction_type, b"OCHIMP")?;
            m.price = read(15, 8);
            if m.kind == 0x95 {
                m.buy_quantity = read(23, 4) as u32;
                m.sell_quantity = read(27, 4) as u32;
                m.indicative_price = read(31, 8);
                m.auction_only_price = read(39, 8);
            } else {
                m.quantity = read(23, 4) as u32;
            }
        }
    }
    Ok(m)
}
