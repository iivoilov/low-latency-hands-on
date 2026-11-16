//! Opt-in telemetry, exclusively called by the book-owning main thread.
use processor_rust::{l1::Quote, monotonic_ns, Processor};
use serde_json::{json, Value};
use std::{
    collections::BTreeMap,
    net::UdpSocket,
    time::{SystemTime, UNIX_EPOCH},
};

pub struct Monitor {
    socket: UdpSocket,
    port: u16,
    session: u64,
    generation: u64,
    heartbeat: u64,
    previous: BTreeMap<String, Vec<u8>>,
    pub dropped: u64,
}
impl Monitor {
    pub fn new(port: u16) -> Result<Self, String> {
        let socket =
            UdpSocket::bind((std::net::Ipv4Addr::LOCALHOST, 0)).map_err(|e| e.to_string())?;
        socket.set_nonblocking(true).map_err(|e| e.to_string())?;
        Ok(Self {
            socket,
            port,
            session: SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map_err(|e| e.to_string())?
                .as_nanos() as u64,
            generation: 0,
            heartbeat: 0,
            previous: BTreeMap::new(),
            dropped: 0,
        })
    }
    pub fn heartbeat(&mut self, processor: &Processor) -> Result<(), String> {
        let now = monotonic_ns();
        if now - self.heartbeat >= 1_000_000_000 {
            self.heartbeat = now;
            self.publish(processor, false, true)?;
        }
        Ok(())
    }
    pub fn publish(
        &mut self,
        processor: &Processor,
        failed: bool,
        force: bool,
    ) -> Result<(), String> {
        let mut levels: BTreeMap<String, (Vec<Value>, Vec<Value>)> = BTreeMap::new();
        for level in processor.book.snapshot() {
            let sides = levels.entry(level.symbol).or_default();
            let side = if level.side == 'B' {
                &mut sides.0
            } else {
                &mut sides.1
            };
            side.push(json!({"price_1e4":level.price_1e4.to_string(), "quantity":level.quantity.to_string(), "orders":level.orders}));
        }
        let mut current = BTreeMap::new();
        for (symbol, state) in &processor.l1.symbols {
            let name = std::str::from_utf8(symbol)
                .map_err(|e| e.to_string())?
                .trim_end_matches(' ')
                .to_string();
            let l = &state.l1;
            let quote = |q: Option<Quote>| {
                q.map(
                    |q| json!({"price_1e4":q.price.to_string(), "quantity":q.quantity.to_string()}),
                )
            };
            let last = l.last.map(|t| json!({"execution_id":t.execution_id.to_string(),"price_1e4":t.price.to_string(),"quantity":t.quantity.to_string(),"trade_date":t.trade_date}));
            let sides = levels.remove(&name).unwrap_or_default();
            let bytes = serde_json::to_vec(&json!({
                "symbol":name,"book_current":processor.book_current() && !failed,
                "trade_history_current":processor.l1.trade_history_current(), "loss_seen":processor.loss_seen(),
                "l1":{"bid":quote(l.bid),"ask":quote(l.ask),"last":last,
                    "trading_status":(l.trading_status as char).to_string(),
                    "reg_sho":(l.reg_sho as char).to_string(),"retail":(l.retail as char).to_string()},
                "l2":{"bids":sides.0,"asks":sides.1}
            })).map_err(|e| e.to_string())?;
            if force || self.previous.get(&name) != Some(&bytes) {
                self.send(&bytes)?;
            }
            current.insert(name, bytes);
        }
        let removed: Vec<String> = self
            .previous
            .keys()
            .filter(|k| !current.contains_key(*k))
            .cloned()
            .collect();
        for name in removed {
            self.send(
                &serde_json::to_vec(&json!({"symbol":name,"removed":true}))
                    .map_err(|e| e.to_string())?,
            )?;
        }
        self.previous = current;
        Ok(())
    }
    fn send(&mut self, bytes: &[u8]) -> Result<(), String> {
        if bytes.len() > 8 * 1024 * 1024 {
            return Err("Monitor symbol snapshot exceeds 8 MiB".into());
        }
        self.generation += 1;
        let parts = bytes.len().div_ceil(1400) as u32;
        for (part, chunk) in bytes.chunks(1400).enumerate() {
            let mut packet = Vec::with_capacity(28 + chunk.len());
            packet.extend_from_slice(b"LLM1");
            packet.extend_from_slice(&self.session.to_be_bytes());
            packet.extend_from_slice(&self.generation.to_be_bytes());
            packet.extend_from_slice(&(part as u32).to_be_bytes());
            packet.extend_from_slice(&parts.to_be_bytes());
            packet.extend_from_slice(chunk);
            if self
                .socket
                .send_to(&packet, (std::net::Ipv4Addr::LOCALHOST, self.port))
                .is_err()
            {
                self.dropped += 1;
            }
        }
        Ok(())
    }
}
