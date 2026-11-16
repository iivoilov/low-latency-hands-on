# Equities PITCH and L1 decisions

The C++ and Rust OS-socket processors decode every U.S. **equities multicast**
PITCH message in the [Cboe U.S. Equities/Options Multicast PITCH specification](https://www.cboe.com/document/tech-spec/document/technical-specifications/cboe-titanium-u.s.-equitiesoptions-multicast-pitch-specification),
revision 2.41.87 (2026-11-10), checked 2026-11-14. This scope covers BYX, BZX,
EDGA and EDGX message layouts; it does not include options-specific messages,
options trade conditions or other regional PITCH protocols. Optional TCP
GRP/spin recovery is documented separately in [recovery](recovery.md).
An options payload can resemble an equities prefix; this receiver must only be
connected to an equities feed. Accepting trailing extensions is not options support.

## Coverage

| Message | Type | Equities prefix bytes | Applied behavior |
| --- | --- | ---: | --- |
| Time | `20` | 6 or 10 | Retain seconds and optional BZX/EDGX epoch seconds |
| Time Reference | `B1` | 18 | Retain midnight, seconds, offset and trade date |
| Unit Clear | `97` | 6 | Clear orders/quotes; preserve trades and symbol metadata |
| Add, long / short / expanded | `21` / `22` / `2F` | 34 / 26 / 41 | Insert order and aggregate L2 |
| Executed | `23` | 26 | Reduce displayed order; record trade at resting price |
| Executed at price/size | `24` | 38 | Replace displayed remainder; record trade at execution price |
| Reduce, long / short | `25` / `26` | 18 / 16 | Reduce displayed quantity |
| Modify, long / short | `27` / `28` | 27 / 19 | Replace price/quantity; zero quantity removes order |
| Delete | `29` | 14 | Remove order |
| Trade, long / short / expanded | `2A` / `2B` / `30` | 41 / 33 / 43 | Record hidden/routed execution; leave L2 unchanged |
| Trade Break | `2C` | 14 | Remove execution; recompute last without reversing L2 |
| End of Session | `2D` | 6 | Mark session ended; reject subsequent sequenced messages |
| Trading Status | `31` | 18 | Retain trading state and Reg SHO indicator |
| Auction Update | `95` | 47 | Retain reference, imbalance and indicative prices |
| Auction Summary | `96` | 27 | Retain auction result |
| Retail Price Improvement | `98` | 15 | Retain buy/sell/both/none indicator |

Short prices have two decimal places; all internal and reported prices are
normalized to units of 0.0001. Six-character symbols are right padded to eight
bytes internally, so they share state with corresponding expanded symbols.
Expanded Adds retain attribution, flags and the customer indicator in their
decoded representation. Attribution and priority do not change aggregated L1.
Framing, documented minimum lengths, symbols, sides, positive Add/execution/
reduction/trade quantities and applicable indicators are validated before book
lookup. Known message extensions are accepted. A partially supplied epoch field
is rejected. Unknown types remain fatal under the project's input policy.

Execution-at-price/size is deliberately different from Reduce: an execution
can exceed the displayed order quantity, and the message's remaining quantity
is authoritative. Its execution price does not move the resting order's price.
Trades do not look up their obfuscated order IDs in the visible book.
Auction summaries may repeat/reprice and overlap individual fills; they are
kept as auction metadata instead of generating duplicate last sales.

Ordinary sequenced heartbeats can reveal gaps. Sequence-zero, zero-message
heartbeats for the selected unit or unit zero are accepted before/after the
session without advancing sequence or establishing a known prefix. Other
unsequenced traffic is outside the equities scope. Sequence rollover remains
UINT32_MAX to 1, excluding zero from sequenced market data.

## L1 output contract

Every successfully applied symbol/order/trade message makes an emit/no-update
decision. Best bid is the highest bid level and best ask is the lowest ask level;
quantities aggregate all live orders at that price. L1 excludes the order count.
A quantity-only change at the best price emits; a deeper change or no-op Modify
does not. The current book is venue-local, not a consolidated NBBO.

An emitted `L1Update` carries the complete current symbol state: optional bid,
optional ask, optional last execution (price, quantity, execution ID, trade date), trading
status, Reg SHO, retail indicator and validity. Missing sides and a missing
last are explicit optionals (`null` in reports), so one-sided/empty books are
representable. A changed-field mask identifies what caused the event:

| Bit value | Changed state |
| ---: | --- |
| 1 | Bid price/quantity/presence |
| 2 | Ask price/quantity/presence |
| 4 | Last execution/presence |
| 8 | Trading status or Reg SHO |
| 16 | Retail indicator |
| 32 | Book or trade-history validity |

Successive trades with the same price and size still emit, since their execution
IDs differ. Execution history keys include the trade date from Time Reference,
so day-specific execution IDs can repeat across dates on BZX/EDGX's longer
sessions. Last carries its date; zero means no date has been supplied (the
BYX/EDGA Time variant supplies none). A Trade Break addresses the current trade
date. Last is preserved across a new Time Reference until an execution or break
changes it; no daily last-price reset is inferred from wall-clock time.
Breaking the latest trade rolls last back to the most recently
received unbroken execution for that symbol. Breaking an older trade preserves
the current last. Breaking the only trade clears last. A break never restores
orders or quantities in L2. An unknown broken ID is fatal with complete history;
after loss/join it is ignored without inventing an execution.

`Processor::set_l1_sink` registers a synchronous callback on the consumer thread
in both languages. The callback runs after the decision's timing endpoint.
Events from warmup and beyond the bounded sample window are still delivered.
A downstream consumer/throttler can coalesce these snapshots. The benchmark
executable uses the default counting sink; it does not write an event stream or
perform live output. Reports contain `l1_decisions`, `l1_updates`,
`l1_no_updates` and `final_l1`, including auction metadata. Unit Clear and loss
can emit additional validity/quote events outside sampled market decisions, so
`l1_updates + l1_no_updates` need not equal `l1_decisions`.
Consumers must not reenter or mutate the Processor from its callback. A fatal
processing error terminates ingestion; callers must invalidate downstream state.
No throttler or external publishing transport is included. Optional
[GRP/spin recovery](recovery.md) uses the existing consumer thread.

## Validity and resource limits

Packet loss immediately emits invalidity for retained symbols. Unit Clear
restores order-book continuity and clears quotes, while preserving last trades
and symbol metadata. It cannot recreate missed executions: the separate
`trade_history_current` flag stays false after loss or a mid-session join for
the rest of this Processor instance in lossy continuation mode. Optional ordered
GRP recovery can preserve the complete prefix; a spin cannot restore execution
history. See [recovery validity](recovery.md#spin-and-validity). A new execution supplies an observed last,
but does not make earlier trade history complete. Run loss history continues to
invalidate benchmarks even after an order reset.

`--max-live-orders` still bounds live orders (default 1,000,000), and also bounds
the number of retained L1 symbols independently. Symbols with no quotes, trades
or metadata are pruned. Trade-only and metadata-only symbols remain available
when their visible books empty, subject to this retained-symbol limit.

`--max-trades` bounds retained unbroken executions (default 1,000,000,
range 1..100,000,000). All unbroken executions are needed for exact last rollback;
there is no silent eviction. Breaking an execution releases its slot. Reaching
the limit is fatal, including in continue mode. A long session may require a
larger limit and memory budget or a fresh instance at the session boundary.
Order/trade hash capacity and timing storage are reserved before reception;
per-node allocations remain measured. C++ uses SipHash-2-4 keys from OS entropy
for order IDs and independently keyed execution-ID/date hashing; Rust uses standard randomized HashMaps.

## Timing and comparison

The start is immediately after successful `recvmsg` in the RX thread. The end
is after decoding, L2 aggregation, trade-history work, L1 comparison and the
emit/no-update decision on the consumer thread. Both emitted and suppressed
updates count toward the bounded sample window after warmup. Status, auction,
retail and trade-break messages are market decisions too. Clock, Unit Clear,
End of Session and heartbeats are not sampled. Updates skipped because an
incomplete book lacks an order/reference are counted separately, without a
latency sample. Old prepared workloads keep their original counts because their
message mix has not changed.

JSON uses `timing_boundary: recvmsg_return_to_message_l1_decision` and
`rx_to_l1_decision`. This interval includes ring wait and earlier messages in
the datagram. Synchronous downstream dispatch follows this message's endpoint;
its work can affect later messages sharing the same RX timestamp. The default
sink performs no dispatch/I/O. Clock reading is included, with no subtraction.

Historical reports keep `rx_to_l2` and their original timing boundary. The remote
runner accepts either schema, records the metric/boundary explicitly, and puts
different boundaries in separate summary rows. New L1 measurements must not be
compared as equivalent to historical L2-only numbers. The
[2026-11-14 RX-to-L1 matrix](experiments/2026-11-14-rx-l1.md) covers the selected
streamer-to-receiver direction, four traffic profiles and five compiler/language
builds: 60/60 valid runs. The 2026-11-15 audit confirms RX CPU 2 and book/L1 CPU 3
in all 60 saved affinity records. Extra reverse runs are archived diagnostics;
they do not qualify the busy streamer as a receiver benchmark.

## Verification

The C++ processor uses `decode_pitch_message_zero_copy` by default. The byte
decoder `decode_pitch_message` and the copying packed decoder
`decode_pitch_message_packed` remain comparison implementations. See the
[original decoder comparison](experiments/2026-11-14-packed-decoder.md)
for the earlier copying results.

`decode_pitch_message_zero_copy(span<uint8_t>)` selects a packed layout
by type and reads it through a `const` reference into writable receive storage.
The C++20 lifetime marker does not initialize or copy bytes. Each view checks
the exact selected layout's extent; it does not form a maximum-sized union over
a short message. References stay inside the call, and the normalized return
value is the same as the other decoders. The packed layouts are a compiler
extension with size/offset/alignment assertions. See the
[three-way benchmark](experiments/2026-11-14-zero-copy-decoder.md).
`Processor::process` requires exclusively owned writable input, provided by the
ring consumer until it releases the slot. The JSON report records
`pitch_decoder: "packed-zero-copy"`. The read-only `OrderBook::apply` convenience
API still uses the byte decoder; the receive path applies an already decoded
message directly to the book.

`processors/tests/equities_messages.tsv` contains fabricated bytes for every
listed layout, including both Time forms. Both native suites decode these same
fixtures and check every truncated prefix and trailing extensions. Behavioral
tests cover one-sided output, quantity changes, deep/no-op suppression, endpoint
placement, short-price conversion, expanded symbols, executions larger than
displayed size, trade rollback, Unit Clear, invalidity, resource limits and
out-of-session heartbeats and trade-date rollover. The shared loopback suite sends all layouts through
the real socket/ring path and checks L2, L1, trades, clocks and metadata in both
reports, alongside the existing 100k-update manifest comparison and loss tests.

Local verification on 2026-11-14:

| Command/check | Result |
| --- | --- |
| Release CMake build with project warning flags | Passed |
| `ctest --test-dir build --output-on-failure` | 7 passed, including loopback multicast |
| `CARGO_TARGET_DIR="$PWD/build/rust-tests" cargo test --manifest-path processors/rust/Cargo.toml --locked` | 16 native tests passed |
| `python3 benchmark/build_rust.py` | Release build passed |
| Shared CLI and multicast suite against the Rust release executable | Passed |
| `python3 -m unittest discover -s benchmark -p 'test_*.py'` | 26 passed |
| Debug UBSan build, `processor_core` and `pitch_l1` | Both passed |
| Combined GCC ASan/UBSan build | Built; full ASan verification unavailable reliably in this environment |

The first socket tests failed because the sandbox forbids socket creation;
they passed when rerun with local loopback access. ASan test runs inside and
outside the sandbox timed out while producing repeated `DEADLYSIGNAL` lines
without an actionable diagnostic. An empty ASan-instrumented `main` also exited
with SIGSEGV with the ASan SIGSEGV handler disabled; the standalone new PITCH/L1
test passed with that setting. This does not establish a clean full ASan run.
UBSan was checked separately. These are correctness checks with synthetic input,
not live-feed certification or new comparative performance measurements.
