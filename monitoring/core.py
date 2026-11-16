"""Clock-injected conflation and bounded, atomic localhost UDP reassembly."""
from dataclasses import dataclass, field
import json
import math
import struct

HEADER = struct.Struct("!4sQQII")
CHUNK = 1400
MAX_FRAME = 8 * 1024 * 1024


def symbol_name(value):
    if not isinstance(value, str) or not 1 <= len(value) <= 8 or any(not 33 <= ord(c) <= 126 for c in value):
        raise ValueError("A symbol needs 1..8 printable ASCII characters without spaces")
    return value


def validate_state(state):
    if not isinstance(state, dict):
        raise ValueError("Expected a symbol snapshot")
    symbol_name(state.get("symbol"))
    if state.get("removed") is True:
        return state
    for key in ("book_current", "trade_history_current", "loss_seen"):
        if type(state.get(key)) is not bool:
            raise ValueError("Missing validity flag")
    l1, l2 = state.get("l1"), state.get("l2")
    if not isinstance(l1, dict) or not isinstance(l2, dict):
        raise ValueError("Expected L1 and L2 snapshots")

    def unsigned(value, maximum=2**64 - 1):
        if not isinstance(value, str) or not value.isascii() or not value.isdigit() or len(value) > 20 or int(value) > maximum:
            raise ValueError("Expected an unsigned integer string")

    for key in ("bid", "ask", "last"):
        if key not in l1:
            raise ValueError("Missing L1 field")
        quote = l1[key]
        if quote is not None:
            if not isinstance(quote, dict):
                raise ValueError("Invalid quote")
            unsigned(quote.get("price_1e4"))
            unsigned(quote.get("quantity"))
            if key == "last":
                unsigned(quote.get("execution_id"))
                if type(quote.get("trade_date")) is not int or not 0 <= quote["trade_date"] <= 2**32 - 1:
                    raise ValueError("Invalid trade date")
    for key in ("trading_status", "reg_sho", "retail"):
        if not isinstance(l1.get(key), str) or len(l1[key]) != 1:
            raise ValueError("Invalid status")
    for side in ("bids", "asks"):
        if not isinstance(l2.get(side), list):
            raise ValueError("Invalid depth")
        previous = None
        for level in l2[side]:
            if not isinstance(level, dict):
                raise ValueError("Invalid level")
            unsigned(level.get("price_1e4"))
            unsigned(level.get("quantity"))
            if type(level.get("orders")) is not int or not 1 <= level["orders"] <= 2**32 - 1:
                raise ValueError("Invalid order count")
            price = int(level["price_1e4"])
            if previous is not None and (price >= previous if side == "bids" else price <= previous):
                raise ValueError("Depth must be sorted best first with unique prices")
            previous = price
    return state


class Reassembler:
    def __init__(self, timeout=1.0, max_pending=64, max_bytes=32 * 1024 * 1024):
        self.timeout, self.max_pending, self.max_bytes = timeout, max_pending, max_bytes
        self.pending = {}
        self.buffered = 0
        self.invalid = self.expired = 0

    def _discard(self, key):
        item = self.pending.pop(key)
        self.buffered -= item[3]

    def expire(self, now):
        for key, (started, _, _, _) in list(self.pending.items()):
            if now - started >= self.timeout:
                self._discard(key)
                self.expired += 1

    def feed(self, packet, now):
        self.expire(now)
        try:
            if not HEADER.size < len(packet) <= HEADER.size + CHUNK:
                raise ValueError("Invalid datagram size")
            magic, source, generation, part, count = HEADER.unpack_from(packet)
            if magic != b"LLM1" or not source or not generation or not 1 <= count <= math.ceil(MAX_FRAME / CHUNK) or part >= count:
                raise ValueError("Invalid fragment header")
            key = (source, generation)
            if key not in self.pending:
                if len(self.pending) >= self.max_pending:
                    self._discard(next(iter(self.pending)))
                    self.expired += 1
                self.pending[key] = [now, count, {}, 0]
            item = self.pending[key]
            if item[1] != count:
                self._discard(key)
                raise ValueError("Inconsistent fragment count")
            chunk = packet[HEADER.size:]
            if part in item[2]:
                if item[2][part] != chunk:
                    self._discard(key)
                    raise ValueError("Conflicting duplicate fragment")
                return None
            if item[3] + len(chunk) > MAX_FRAME or self.buffered + len(chunk) > self.max_bytes:
                self._discard(key)
                raise ValueError("UDP assembly memory limit")
            item[2][part] = chunk
            item[3] += len(chunk)
            self.buffered += len(chunk)
            if len(item[2]) == count:
                body = b"".join(item[2][i] for i in range(count))
                self._discard(key)
                state = validate_state(json.loads(body))
                return source, generation, state
        except (ValueError, TypeError, KeyError, UnicodeError, RecursionError):
            self.invalid += 1
        return None


@dataclass
class Subscription:
    watches: tuple
    interval: float
    expires: float
    last: dict = field(default_factory=dict)
    sent: dict = field(default_factory=dict)
    seq: int = 0


class Throttler:
    def __init__(self, stale_after=3.0, max_symbols=10000, max_subscribers=256):
        self.stale_after, self.max_symbols, self.max_subscribers = stale_after, max_symbols, max_subscribers
        self.source = 0
        self.states = {}
        self.subscribers = {}
        self.ignored = 0

    def ingest(self, source, generation, state, now):
        # Exporter startup IDs are UNIX nanoseconds. One producer per instance;
        # an older producer cannot overwrite a newly restarted producer.
        if source < self.source:
            self.ignored += 1
            return
        if source > self.source:
            self.source = source
            self.states.clear()
        name = state["symbol"]
        old = self.states.get(name)
        if old and generation <= old[0]:
            self.ignored += 1
            return
        if name not in self.states and len(self.states) >= self.max_symbols:
            self.ignored += 1
            return
        # Keep a tombstone's generation so reordered packets cannot resurrect it.
        self.states[name] = (generation, now, None if state.get("removed") else state)

    def subscribe(self, subscriber, l1, l2, interval_ms, now):
        if subscriber in self.subscribers:
            raise ValueError("Subscription already exists")
        if len(self.subscribers) >= self.max_subscribers:
            raise ValueError("Subscriber limit reached")
        if type(interval_ms) is not int or not 10 <= interval_ms <= 60000:
            raise ValueError("interval_ms must be 10..60000")
        if not isinstance(l1, list) or not isinstance(l2, list) or len(l1) + len(l2) > 256:
            raise ValueError("Watch at most 256 symbol/stream pairs")
        watches = tuple(sorted({("l1", symbol_name(s)) for s in l1} | {("l2", symbol_name(s)) for s in l2}))
        self.subscribers[subscriber] = Subscription(watches, interval_ms / 1000, now + 30)

    def value(self, stream, symbol, now):
        item = self.states.get(symbol)
        state = item[2] if item else None
        stale = not item or now - item[1] >= self.stale_after
        result = {"available": state is not None, "source_stale": stale, "state": None}
        if state is not None:
            result["state"] = {key: state[key] for key in ("book_current", "trade_history_current", "loss_seen")}
            result["state"]["data"] = state[stream]
        return result

    def candidates(self, now):
        for subscriber, sub in list(self.subscribers.items()):
            if now >= sub.expires:
                del self.subscribers[subscriber]
                continue
            for key in sub.watches:
                value = self.value(*key, now)
                if key not in sub.last or (value != sub.last[key] and now - sub.sent[key] >= sub.interval):
                    yield subscriber, sub, key, value

    def event(self, sub, key, value):
        return {"type": "snapshot" if key not in sub.last else "update", "stream": key[0], "symbol": key[1], "seq": sub.seq + 1, **value}

    def delivered(self, sub, key, value, now):
        sub.last[key], sub.sent[key] = value, now
        sub.seq += 1
