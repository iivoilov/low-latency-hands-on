# Human-facing market monitor

Both processors can export packet-final L1/L2 state over localhost UDP. A Python
throttler retains current state and publishes a separate Valkey Pub/Sub stream
for each subscriber. A separate FastAPI app makes a real broker subscription
for each browser WebSocket and serves the watchlist/depth UI. Python 3.11+ is
required; the browser UI is plain HTML/CSS/JavaScript with no build step or CDN.

```text
PITCH multicast → processor_cpp OR processor_rust
                          │ opt-in localhost UDP snapshots
                          ▼
                     throttler ← HTTP subscription/lease control ← app service
                          │                                         │
                          └→ Valkey: subscriber's private channel ────┘
                                                                    │
                                                           HTTP + WebSocket
                                                                    ▼
                                                                 browser
```

## Run locally

From the repository root, build the processors with the existing CMake/Rust
commands. Install the separate monitoring dependencies in an ignored environment:

```sh
python3 -m venv build/monitor-venv
build/monitor-venv/bin/python -m pip install -r monitoring/requirements.txt
```

Use an already running Valkey server, or start a disposable local instance:

```sh
valkey-server --bind 127.0.0.1 --port 6379 --save "" --appendonly no
```

The broker URL comes from `MONITOR_VALKEY_URL` in **both** services. It defaults
to `redis://127.0.0.1:6379/0`; the URL scheme is the client protocol, compatible
with Valkey. `rediss://` enables TLS. Keep credentials in the environment, outside
source control. For a broker on another lab host, an SSH tunnel avoids changing
its bind address or exposing a new port:

```sh
ssh -N -L 6379:127.0.0.1:6379 <valkey-host>
```

Start the two services with one command:

```sh
build/monitor-venv/bin/python -m monitoring
```

Open **http://127.0.0.1:8767**. The launcher starts the throttler on HTTP port
8766 and UDP port 8765, then the app on port 8767. Ctrl-C stops both. Ports can
be changed with `--port`, `--control-port`, and `--udp-port`. Start **one** of
the following processor commands on the same host as the throttler:

```sh
build/processor_cpp --continuous --on-loss continue --monitor-port 8765
# Or:
build/rust-o3/processor_rust --continuous --on-loss continue --monitor-port 8765
```

Then start `play_feed` with a prepared PCAP and the same group/interface/port
as that processor. The standard loopback defaults work unchanged. Prepared
files and processor initialization/sample settings remain as described in the
[processor commands](../processors/README.md). A completed file leaves its last
book visible; the exporter continues reannouncing it while the processor runs.
Stopping the processor marks its retained display stale after three seconds.

### Repeat a prepared feed until Ctrl-C

Keep the throttler/web services running and stop the separately started processor
first. From the repository root, run:

```sh
feed_dir=simulator/play_files/sp500-20261113-seed44
python3 -m monitoring.replay \
  "$feed_dir/pitch-v3-1m-base8m-peak20m-b10ms-e100ms_sym503_seed44.pcap"
```

The launcher defaults to `build/monitor-cpp/processor_cpp` and
`build/monitor-cpp/play_feed`; override them with `--processor PATH` and
`--player PATH`. It reads unit, initialization and sample counts from the
same-stem JSON manifest. `--monitor-port` selects the throttler's UDP port
(8765 by default), and `--interface` selects the multicast interface (loopback
by default). It uses no additional Python dependencies. `--cycles 2` optionally
limits the number of passes; otherwise replay continues until Ctrl-C.

Each pass starts a fresh continuous processor, waits for READY, then plays the
file once. After playback it allows 250 ms for trailing packets, stops that
processor, and starts the next pass. The monitor recognizes the new producer
session and replaces its state. The displayed book briefly resets/rebuilds at
each boundary, and the same synthetic price trajectory repeats. Ctrl-C stops
both owned children while leaving the monitor services running. An unexpected
child failure stops the launcher instead of retrying it.

A plain `while ... play_feed ...` loop against one continuous processor would
reuse sequence numbers and be discarded as stale data. Unit Clear alone also
preserves executions, so changing sequence numbers is insufficient to restart
the whole feed. The launcher preserves the prepared payloads and processor
protocol semantics by giving each replay its own process/session.

This is a human-monitor demo loop, not a benchmark: telemetry adds overhead,
loss remains visible in the UI and per-cycle counters, and the 250 ms pause is
not a verified processing-completion barrier. Only one processor producer should
feed a given monitor instance. Single-pass `play_feed` behavior is unchanged.

Local verification of the 503-stock 8/20 Mbps file completed two cycles but
reported substantial ring overflow and incomplete books with monitoring enabled;
see `build/monitor-replay-503.log`. Cycling does not resolve exporter overhead.
The smaller fabricated replay test checks two complete, loss-free passes,
distinct producer sessions, repeated trade IDs, Ctrl-C, and failure cleanup:

```sh
ctest --test-dir build/monitor-cpp -R '^monitoring_(core|replay)$' --output-on-failure
python3 -m monitoring.tests.test_replay build/monitor-rust/processor_rust \
  build/monitor-cpp/prepare_feed build/monitor-cpp/play_feed
```

The UI discovers symbols, initially subscribes to the first six, and selects
one for L2. Enter a comma-separated watchlist, select a depth security and
interval, then choose **Apply subscriptions**. Selecting a quote row requests
that security's L2 stream. Each browser tab has its own subscription and rate.
The L2 stream contains all aggregated levels; the UI displays the best 20 on
each side. Prices use integer ten-thousandths of a dollar. Prices, quantities
and execution IDs travel as decimal strings, and the UI uses `BigInt` to avoid
JavaScript precision loss. Size means shares, not order count.

## Throttle semantics

Default interval is 1000 ms, configurable from 10 to 60000 ms per subscriber.
L1 and L2 use independent timers per security:

* Subscription sends an immediate full snapshot for every requested stream and
  security, including explicit unavailable state for a symbol not seen yet.
* After an idle interval, the first change publishes immediately.
* Within the interval, changes collapse into current state. Once the interval
  expires, publish that state if it differs from the last delivered state.
* If a change reverts to the last delivered state, no update is necessary.
* Unchanged state produces no market-data events. Reannouncements and lease
  heartbeats do not create price updates. A deeper L2 change does not create L1.

The publisher polls due work every 10 ms; event-loop scheduling, broker and
WebSocket delivery can add delay. Timers use `time.monotonic()`, and their next
interval starts after broker publication completes. Initial snapshots establish
the first interval. Full state is sent for each conflated event; it is not a
changed-field mask or a sequence of every intervening execution.

Book integrity, retained trade-history integrity and historical feed loss are
separate flags. Unit Clear can repair L2 without repairing missed trades or
erasing run loss. Exporter freshness is also separate: it indicates a live
processor, not that trades are occurring. Missing/old UDP fragments never
partially modify displayed depth. A fresh complete snapshot replaces previous
state atomically. UDP is lossy; periodic full snapshots repair missed telemetry
within the next successful reannouncement. They do not repair the processor's
underlying feed loss.

## Service interfaces

Services can run independently:

```sh
build/monitor-venv/bin/python -m monitoring.throttler --udp-port 8765 --port 8766
MONITOR_THROTTLER_URL=http://127.0.0.1:8766 \
  build/monitor-venv/bin/python -m monitoring.app --host 127.0.0.1 --port 8767
```

The throttler always binds its control/UDP sockets to loopback. The app can
bind a trusted private address with `--host`; no user authentication is
implemented. Keep the control API private. This change does not deploy services,
configure TLS, modify remote hosts or install anything on them. Run one
throttler worker and one active processor producer per instance.

The app exposes `GET /api/health`, `GET /api/symbols` and `/ws`. On a new
WebSocket, send one configuration object:

```json
{"l1":["AAPL","MSFT"],"l2":["AAPL"],"interval_ms":1000}
```

The response starts with `{"type":"subscribed", ...}`. Data events contain
`type` (`snapshot` or `update`), `stream`, `symbol`, per-subscription `seq`,
`available`, `source_stale` and `state`. When available, `state` holds
`book_current`, `trade_history_current`, `loss_seen` and `data`. L1 `data` has
`bid`, `ask`, `last` and trading-status/Reg SHO/retail indicators; L2 has sorted
`bids`/`asks`, each with `price_1e4`, `quantity` and `orders`. Absent quotes/trades
are JSON null. `heartbeat` messages keep the browser informed that the bridge is
alive; they are not data updates. To change configuration, reconnect.

For direct subscribers, subscribe to Valkey channel
`low_latency:monitor:<32 lowercase hex characters>` and **read the SUBSCRIBE
acknowledgment first**, then call `POST /subscriptions/<same ID>` on the
throttler with the configuration above. This ordering prevents a lost initial
snapshot. Renew via `POST /subscriptions/<ID>/heartbeat` at least every 30 seconds
(the app renews every five), and `DELETE /subscriptions/<ID>` to close. Limits
are 256 subscribers, 256 symbol/stream pairs per subscriber and 10000 retained
securities, including removal tombstones.

Valkey Pub/Sub has [at-most-once delivery](https://valkey.io/topics/pubsub/).
The bridge checks stream sequence continuity; gaps, expired subscriptions,
broker/throttler failures, or a browser that cannot accept a send within two
seconds close the connection. The browser retries and gets new snapshots.
Subscribers are removed on disconnect, zero broker listeners, broker publish
failure, or lease expiration. This is live monitoring without durable replay.

## Exporter wire format and ownership

`--monitor-port` requires `--continuous` and a nonzero port, and always targets
127.0.0.1. The option is off by default in both languages. No book worker or
shared mutable book state is introduced. After each processed datagram, the
existing book-owning thread takes read-only snapshots, serializes changed
security state, and attempts nonblocking UDP sends. It also resends full state
once per second, even during idle periods. Temporary allocations, book scans
and UDP I/O are intentional costs of this optional visualization path. They
can increase subsequent ring wait and feed loss; **do not use monitoring-enabled
runs as the processor benchmark baseline**. Reports identify this with
`monitoring_enabled`; UDP send failures are counted on stderr at shutdown.

In Rust, the exporter borrows `&Processor` after packet processing. While that
immutable borrow exists, the compiler prevents mutation through the same
processor reference. The generated `Vec`/JSON snapshots own their data; no
references into the live book cross a thread or await point. The next packet
can safely mutate the book after the call returns. C++ uses the corresponding
const reference from the same owning thread. Default processor timing boundaries
and two-thread operation remain unchanged.

Each security snapshot is UTF-8 JSON with top-level `symbol`, three validity
flags, `l1`, and `l2`. A removed symbol uses `{"symbol":"...","removed":true}`.
Snapshots larger than 1400 bytes are fragmented at the application layer:

| Offset | Bytes | Meaning |
| --- | ---: | --- |
| 0 | 4 | ASCII `LLM1` |
| 4 | 8 | Exporter startup UNIX nanoseconds, session ID |
| 12 | 8 | Increasing snapshot generation in this session |
| 20 | 4 | Zero-based fragment index |
| 24 | 4 | Total fragment count |
| 28 | 1..1400 | Slice of serialized JSON |

Header integers use network byte order. Every datagram is at most 1428 bytes,
below the 1472-byte IPv4 UDP limit. Reassembly accepts out-of-order fragments,
ignores identical duplicates and rejects conflicting fragments, malformed data,
invalid field types, and oversized snapshots. Incomplete assemblies expire
after one second; there are at most 64 assemblies, 32 MiB buffered fragments,
and 8 MiB per symbol snapshot. Old per-symbol generations are ignored.
A newer exporter session clears cached securities, so a restart cannot keep
old symbols marked current. Startup session IDs assume a nondecreasing host
wall clock; monitoring intervals and freshness use monotonic time.

## Verify

Core tests require only the Python standard library and are registered with
CTest. The integration suite requires the monitoring dependencies, a local
Valkey executable and both built processors. It starts isolated temporary
services on loopback, disables broker persistence, uses fabricated PITCH packets,
and shuts everything down afterward:

```sh
python3 -m unittest monitoring.tests.test_core -v
build/monitor-venv/bin/python -m monitoring.tests.test_pipeline \
  --valkey-server /path/to/valkey-server \
  --cpp build/processor_cpp --rust build/rust-o3/processor_rust
```

This tests initial snapshots, independent rates, final-state conflation, L2-only
changes, exact large execution IDs, partial books, Unit Clear, stale exporters,
subscriber cleanup, broker interruption and reconnect snapshots. With installed
Playwright and `/usr/bin/chromium`, `--browser` also exercises a real subscription
change and checks desktop/mobile rendering without page errors or mobile page
overflow. Screenshots go under `build/monitor-preview/`. Socket-restricted
sandboxes need a socket-enabled environment for these integration checks.
