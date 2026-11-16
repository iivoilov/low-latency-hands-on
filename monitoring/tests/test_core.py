import copy
import json
import unittest

from monitoring.core import CHUNK, HEADER, Reassembler, Throttler, validate_state


def state(quantity="100"):
    return {"symbol": "TEST", "book_current": True, "trade_history_current": True, "loss_seen": False,
            "l1": {"bid": {"price_1e4": "100000", "quantity": quantity}, "ask": None, "last": None,
                   "trading_status": "T", "reg_sho": "0", "retail": "N"},
            "l2": {"bids": [{"price_1e4": "100000", "quantity": quantity, "orders": 1}], "asks": []}}


def fragments(value, generation=1, source=1):
    data = json.dumps(value).encode()
    chunks = [data[i:i + CHUNK] for i in range(0, len(data), CHUNK)]
    return [HEADER.pack(b"LLM1", source, generation, i, len(chunks)) + part for i, part in enumerate(chunks)]


class ThrottlerTests(unittest.TestCase):
    def setUp(self):
        self.engine = Throttler(stale_after=100)
        self.engine.ingest(1, 1, state(), 0)
        self.engine.subscribe("a", ["TEST"], ["TEST"], 1000, 0)

    def flush(self, now):
        events = []
        for subscriber, sub, key, value in self.engine.candidates(now):
            events.append((subscriber, self.engine.event(sub, key, value)))
            self.engine.delivered(sub, key, value, now)
        return events

    def test_snapshot_then_leading_and_trailing_edge(self):
        initial = self.flush(0)
        self.assertEqual([e[1]["type"] for e in initial], ["snapshot", "snapshot"])
        self.assertEqual([e[1]["seq"] for e in initial], [1, 2])
        self.engine.ingest(1, 2, state("110"), .2)
        self.assertEqual(self.flush(.2), [])
        self.engine.ingest(1, 3, state("120"), .8)
        self.assertEqual(self.flush(.999), [])
        trailing = self.flush(1)
        self.assertEqual(len(trailing), 2)
        self.assertEqual(trailing[0][1]["state"]["data"]["bid"]["quantity"], "120")
        self.assertEqual(self.flush(4), [])  # Quiet feeds do not get timer updates.
        self.engine.ingest(1, 4, state("130"), 4.1)
        self.assertEqual(len(self.flush(4.1)), 2)  # Leading edge after idle.

    def test_subscriber_timers_and_securities_are_independent(self):
        other = state(); other["symbol"] = "OTHER"
        self.engine.ingest(1, 2, other, 0)
        self.engine.subscribe("b", ["TEST", "OTHER"], [], 250, .5)
        self.flush(.5)
        self.engine.ingest(1, 3, state("120"), .6)
        self.assertEqual(self.flush(.7), [])
        events = self.flush(.75)
        self.assertEqual([(s, e["symbol"]) for s, e in events], [("b", "TEST")])
        self.assertEqual([s for s, _ in self.flush(1.5)], ["a", "a"])
        other = copy.deepcopy(other)
        other["l1"]["bid"]["quantity"] = "333"
        self.engine.ingest(1, 4, other, 1.6)
        self.assertEqual(self.flush(1.6)[0][1]["symbol"], "OTHER")

    def test_reversion_and_deep_l2_only_change(self):
        self.flush(0)
        self.engine.ingest(1, 2, state("200"), .1)
        self.engine.ingest(1, 3, state(), .2)
        self.assertEqual(self.flush(1), [])
        changed = state()
        changed["l2"]["bids"].append({"price_1e4": "90000", "quantity": "50", "orders": 1})
        self.engine.ingest(1, 4, changed, 1.1)
        events = self.flush(1.1)
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0][1]["stream"], "l2")

    def test_stale_refresh_loss_clear_and_restart(self):
        self.engine.stale_after = 3
        self.flush(0)
        self.assertTrue(all(e[1]["source_stale"] for e in self.flush(3)))
        self.engine.ingest(1, 2, state(), 3.1)
        self.assertEqual(self.flush(3.1), [])
        self.assertFalse(self.flush(4)[0][1]["source_stale"])
        dirty = state(); dirty.update(book_current=False, trade_history_current=False, loss_seen=True)
        self.engine.ingest(1, 3, dirty, 5)
        self.assertFalse(self.flush(5)[0][1]["state"]["book_current"])
        dirty = copy.deepcopy(dirty); dirty["book_current"] = True
        self.engine.ingest(1, 4, dirty, 6)
        event = self.flush(6)[0][1]
        self.assertTrue(event["state"]["loss_seen"])
        self.assertFalse(event["state"]["trade_history_current"])
        self.engine.ingest(2, 1, state(), 7)
        self.engine.ingest(1, 999, dirty, 7)
        self.assertTrue(self.flush(7)[0][1]["state"]["trade_history_current"])

    def test_unknown_remove_reorder_and_lease(self):
        self.engine.subscribe("b", ["UNKNOWN"], [], 1000, 0)
        self.assertFalse(self.flush(0)[-1][1]["available"])
        self.engine.ingest(1, 4, {"symbol": "TEST", "removed": True}, 1)
        self.engine.ingest(1, 3, state(), 1)
        self.assertFalse(self.flush(1)[0][1]["available"])
        self.flush(30)
        self.assertEqual(self.engine.subscribers, {})

    def test_reannouncements_and_actual_delivery_clock(self):
        self.flush(0)
        self.engine.ingest(1, 2, state(), 1)
        self.assertEqual(self.flush(1), [])
        self.engine.ingest(1, 3, state("200"), 1.1)
        candidate = next(self.engine.candidates(1.1))
        _, sub, key, value = candidate
        self.engine.delivered(sub, key, value, 2)  # Slow publication completed later.
        self.engine.ingest(1, 4, state("300"), 2.1)
        self.assertNotIn(key, [candidate[2] for candidate in self.engine.candidates(2.9)])

    def test_bounds(self):
        for interval in (0, 9, 60001, True):
            with self.assertRaises(ValueError):
                self.engine.subscribe("b", [], [], interval, 0)
        with self.assertRaises(ValueError):
            self.engine.subscribe("b", ["TOO-LONG-SYMBOL"], [], 1000, 0)
        engine = Throttler(max_symbols=1, max_subscribers=1)
        engine.ingest(1, 1, state(), 0)
        other = state(); other["symbol"] = "OTHER"
        engine.ingest(1, 2, other, 0)
        self.assertEqual(len(engine.states), 1)
        engine.subscribe("a", [], [], 1000, 0)
        with self.assertRaises(ValueError):
            engine.subscribe("b", [], [], 1000, 0)


class DatagramTests(unittest.TestCase):
    def test_reordered_fragments_duplicates_and_expiration(self):
        value = state()
        value["l2"]["bids"] = [{"price_1e4": str(100000 - i), "quantity": "100", "orders": 1} for i in range(80)]
        assembler = Reassembler()
        packets = fragments(value)
        self.assertGreater(len(packets), 2)
        self.assertIsNone(assembler.feed(packets[-1], 0))
        self.assertIsNone(assembler.feed(packets[-1], .1))
        for packet in packets[:-2]:
            self.assertIsNone(assembler.feed(packet, .2))
        result = assembler.feed(packets[-2], .3)
        self.assertEqual(result, (1, 1, value))
        self.assertEqual(assembler.buffered, 0)
        assembler.feed(packets[0], 1)
        assembler.expire(2)
        self.assertEqual(assembler.expired, 1)
        self.assertEqual(assembler.buffered, 0)

    def test_malformed_and_memory_limits(self):
        assembler = Reassembler(max_pending=1, max_bytes=1500)
        self.assertIsNone(assembler.feed(b"not telemetry", 0))
        bad = state(); bad["l1"]["bid"]["price_1e4"] = "-1"
        self.assertIsNone(assembler.feed(fragments(bad)[0], 0))
        self.assertEqual(assembler.invalid, 2)
        assembler.feed(HEADER.pack(b"LLM1", 1, 1, 0, 2) + b"a" * 1400, 0)
        assembler.feed(HEADER.pack(b"LLM1", 1, 1, 1, 2) + b"a" * 1400, 0)
        self.assertEqual(assembler.buffered, 0)
        self.assertEqual(assembler.invalid, 3)
        assembler.feed(HEADER.pack(b"LLM1", 1, 2, 0, 2) + b"a", 0)
        assembler.feed(HEADER.pack(b"LLM1", 1, 3, 0, 2) + b"a", 0)
        self.assertEqual(len(assembler.pending), 1)
        self.assertEqual(assembler.expired, 1)

    def test_large_integers_and_invalid_depth(self):
        value = state(str(2**64 - 1))
        self.assertEqual(validate_state(value), value)
        value["l2"]["bids"] *= 2
        with self.assertRaises(ValueError):
            validate_state(value)


if __name__ == "__main__":
    unittest.main()
