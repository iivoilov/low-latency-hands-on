# Low-latency programming experiments

This project explores how to process incoming data with low and predictable
latency. I use a market-data receiver as the test case: it receives messages
about orders being added, changed, executed or cancelled, then updates an
in-memory view of the market. The experiments compare C++ and Rust, compiler
settings, thread placement and data structures, and measure where time is spent
between the network card and the completed update.

The goal is to understand which choices improve latency, especially the slowest
updates, while preserving correct results and detecting lost data. The repository
contains the implementations, repeatable synthetic workloads, benchmark tools
and experiment reports.

**Latest workload comparison, 16 November 2026:** the C++ and Rust receivers
maintain L2 and generate L1 bid/ask/last snapshots across 503 stocks. Eighteen
separate-host trials completed without detected loss and matched independent
L1/L2 wire oracles. Median per-run p50 was **1.35–1.51 µs** and p99.9 was
**3.57–4.52 µs** across the tested builds and profiles. These intervals start
after `recvmsg` returns and end at each message's L1 emit/no-update decision.
Earlier L2-only timings use a different endpoint and stay in historical reports.

Optional [GRP/spin recovery](docs/recovery.md) and a
[per-subscriber market monitor](monitoring/README.md) are available in both
languages. They remain disabled in benchmark comparisons.

## What the program does

An order book tracks outstanding buy and sell orders. An **L2 book** aggregates
them into quantities available at each price. This makes a useful latency test:
every incoming event requires decoding, finding an order and updating the
relevant price levels. The current processors also track last trades, trade
breaks and symbol metadata, and decide whether each message changes L1.

The experiment has three parts:

1. **Prepare a workload.** Generate a repeatable synthetic feed using a subset
   of the CBOE PITCH protocol, with steady traffic and periodic bursts.
   Save the expected final book so the receiver's result can be checked.
2. **Replay it over a network.** A separate machine sends the prepared packets
   using UDP multicast, which delivers the same stream to subscribed receivers.
3. **Process and measure.** A C++ or Rust receiver applies the updates, records
   latency and loss. The benchmark checks its final book against the expected result.

The current receivers use ordinary Linux UDP sockets. Each has a receive thread
and a book-update thread, connected by a bounded queue of whole packets:

```text
Network card → Linux network stack → receive thread → packet queue → book-update thread
                                     |                                            |
                                     +──── measured application latency ──────────+
```

The receive thread timestamps each packet immediately after the socket receive
call returns. The other thread timestamps each message's L1 emit/no-update
decision. The interval includes queue waiting, decoding and book updates, including earlier
messages in the same packet.

## Methods used to keep latency low

- **Keep preparation and reporting outside processing.** Replay files are loaded
  before sending; queue and timing storage and hash-table capacity are reserved
  before reception. Results are written afterward. Book-node allocations still
  occur during processing and are included in the timings.
- **Give one thread ownership of the book.** A single-producer/single-consumer
  queue transfers whole packets without a mutex; the book-update thread owns
  all order and price-level state.
- **Control CPU placement.** Pin the two threads to separate physical cores
  sharing the last-level cache, reducing the observed cost and variability of
  passing packets between them.
- **Keep correctness and resource limits explicit.** Validate input, bound queues
  and live orders, and detect sequence gaps. Benchmark runs stop on loss; only
  complete runs with matching final books enter successful latency summaries.
  The current C++ order lookup also uses secret-keyed SipHash to resist deliberately
  engineered hash collisions, with its measured cost retained in the results.

## What was checked and learned

| Experiment | Finding |
| --- | --- |
| [GCC vs Clang, O2 vs O3](docs/experiments/2026-11-12-cpp-os.md) | No consistent O3 advantage in the initial trials. Uncontrolled thread placement limited compiler comparisons. |
| [Thread placement and shared cache](docs/experiments/2026-11-12-cpu-affinity.md) | On an earlier C++ version, separate cores sharing cache narrowed the p99.9 range from 0.59–1.18 µs unpinned to 0.57–0.62 µs. Across cache domains, median p99.9 rose to 1.33 µs. |
| [C++ and Rust implementations](docs/experiments/2026-11-13-siphash.md) | Both achieved sub-microsecond median application latency. Rust had a slightly lower median; C++ had a slightly lower p99.9 in the latest profiles. Their containers and hashing differ, so this does not establish a general language ranking. |
| [C++ hash protection](docs/experiments/2026-11-13-siphash.md) | SipHash replaced an earlier scheme that could reject valid collisions. Median latency increased by 80 ns, about 24–25%; SipHash is the selected C++ baseline. |
| [Reversing sender and receiver](docs/experiments/2026-11-12-reverse-direction.md) | Only the lowest-rate profile completed. Separate captures found missing-packet counts matching sender-queue drops; incomplete runs were excluded. |
| [NIC hardware receive timestamps](docs/experiments/2026-11-12-hardware-rx.md) | On the separate EPYC/X550 receiver, NIC-to-application p50 was 43–44 µs and p99.9 was 0.893–1.547 ms. This exposes receive-path delay that the application timer excludes. |

The affinity results used an earlier implementation. They explain the selected
CPU placement; they are not the timings of the current C++ baseline.

## Latest configuration and timings

The selected direction uses a separate EPYC sender and a **Ryzen 9 9950X
receiver, Debian 13, Linux 6.12.117 and 1 Gbps r8169 Ethernet**. RX CPU 2 and
book/L1 CPU 3 are separate physical cores sharing L3; startup runs on CPU 3
and the observer on CPU 15. These CPU choices are specific to this receiver.
The busy EPYC host is not a qualified receiver baseline.

C++ uses keyed SipHash-2-4 with `unordered_map` orders and `map` price levels;
Rust uses `HashMap`/`RandomState` and `BTreeMap`. The C++ decoder reads packed
wire layouts from exclusively owned packet storage and preserves the bytes.
Both reserve capacity for one million live orders and bound retained executions.

The frozen workload has **503 symbols, 30,180 initialization adds and 1,000,000
measured updates**, seed 44, with dated vendor reference prices. S1 uses
8/20 Mbps with 10 ms bursts every 100 ms; S2 uses the same rates with 25 ms
bursts every 250 ms. Rates count PITCH UDP payload bits. Activity is synthetic,
not fitted to exchange traffic. The vendor snapshot and generated workloads
are not distributed; later downloads are different inputs.

Times are **microseconds**, medians of three per-run percentiles. All 18 baseline
trials passed the final L1/L2, sample-count, loss and placement checks.

| Profile | Build | p50 | p99 | p99.9 |
| --- | --- | ---: | ---: | ---: |
| S1 | GCC 14.2 O3 | 1.490 | 3.220 | 3.920 |
| S1 | Clang 19.1 O3 | 1.350 | 2.910 | 3.570 |
| S1 | Rust 1.99.1 release | 1.510 | 3.230 | 4.520 |
| S2 | GCC 14.2 O3 | 1.480 | 3.200 | 3.880 |
| S2 | Clang 19.1 O3 | 1.360 | 2.930 | 3.590 |
| S2 | Rust 1.99.1 release | 1.510 | 3.250 | 4.510 |

These are the controls in the [503-stock report](docs/experiments/2026-11-16-sp500.md),
before the subsequent ring-cache/quantity edits. One GCC S1 run reached
5.250 µs at p99.9 and remains in the results. Three repetitions, shared OS work
and limited offered load do not establish a language ranking or maximum throughput.

The [subsequent independent-change comparison](benchmark/README.md#native-ring-and-quantity-comparison-2026-11-16)
retains 54/54 new native trials and reuses those 18 controls after checking runtime
source equivalence. Rust quantity-only has a small gain; ring-only and combined
results are mixed. Current source includes both changes; no overall speedup from
the combination is claimed. WSL2 results remain separate.

Application timings exclude NIC/driver/kernel delivery before the receive call
returns. Earlier hardware timestamps came from a separate EPYC/X550 RX probe;
their percentiles cannot be added to these figures. Combined NIC-to-L1 latency
remains unmeasured. [AF_XDP checks](docs/experiments/2026-11-13-xdp-rx-timing.md)
collected no receive or latency samples; DPDK remains planned.

## Explore or reproduce

- [Build and run](docs/usage.md): local checks and separate-host experiments.
- [Experiment reports](docs/experiments/README.md): hardware, methods, outcomes
  and reviewed JSON/CSV result tables.
- [Workload recipes](simulator/play_files/README.md): generate the routine 503-stock set or historical three-stock profiles.
- [Project structure](docs/structure.md) and [processor design](processors/README.md).
- [Protocol scope](docs/market-data.md): equities message coverage and generator subset.

Generated workloads and raw captures are not distributed. Historical synthetic
profiles can be regenerated from the recipes. Reproducing the exact 503-stock
bytes also requires the original vendor input snapshot; a new download changes it.

## License

[MIT](LICENSE). SipHash-derived code includes the
[upstream CC0 dedication](docs/licenses/SipHash-CC0.txt).
