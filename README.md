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

**Latest outcome, 13 November 2026:** with two processing threads pinned to
separate cores sharing a CPU cache, the current C++ and Rust implementations
complete an update in **0.36–0.41 µs at the median** and **1.04–1.20 µs at
p99.9**, across the tested traffic profiles. These figures measure the interval
from receiving a packet in the application to updating the book. The earlier
NIC-to-application interval was measured separately on different hardware and
was much larger.

## What the program does

An order book tracks outstanding buy and sell orders. An **L2 book** aggregates
them into quantities available at each price. This makes a useful latency test:
every incoming event requires decoding, finding an order and updating the
relevant price levels.

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
call returns. The other thread timestamps each completed market update. The
interval includes queue waiting, decoding and book updates, including earlier
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

The latest comparison used a separate sender and a **Ryzen 9 9950X receiver
running Debian 13 over 1 Gbps Ethernet**. Receive CPU 2 and book-update CPU 3
are separate physical cores sharing the last-level cache. C++ used GCC 14.2,
C++20 and O3; Rust used rustc 1.99.1 with release optimization level 3.

The current C++ book uses `unordered_map` with SipHash-2-4 for orders and `map`
for price levels. Rust uses its standard `HashMap` with `RandomState` and
`BTreeMap`. Both reserve hash-table capacity for a limit of **one million live
orders**. The timing workload itself has approximately **180 live orders**
across three symbols; large-book capacity was checked separately in tests.

Each run measures **1,000,000 updates after initialization**. Four profiles
vary the background rate from 4 to 12 Mbps and the duration/frequency of bursts
at 20 Mbps. Rates count UDP payload bits. Exact profiles and build settings are
in the [latest experiment report](docs/experiments/2026-11-13-siphash.md).

All times below are **microseconds (µs)**, reported as medians of three per-run
percentiles. **p50** is the median update; **p99** and **p99.9** describe the
slower tail: 99% and 99.9% of sampled updates finish within those times in a run.

| Profile | C++ p50 | C++ p99 | C++ p99.9 | Rust p50 | Rust p99 | Rust p99.9 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| P1 | 0.400 | 0.810 | 1.070 | 0.360 | 0.860 | 1.200 |
| P2 | 0.410 | 0.810 | 1.090 | 0.370 | 0.850 | 1.110 |
| P3 | 0.400 | 0.740 | 1.040 | 0.360 | 0.850 | 1.060 |
| P4 | 0.400 | 0.780 | 1.070 | 0.360 | 0.870 | 1.130 |

All **36 trials** completed with matching final books and no detected loss:
12 current C++, 12 Rust and 12 previous C++ controls. No attempt was retried
or excluded. [Per-trial data and methods](docs/experiments/2026-11-13-siphash.md)
include achieved rates, queue wait and outliers.

These are observations from small synthetic books and three repetitions per
condition. p99.9 is not a maximum: the largest individual application interval
across the full comparison was 265.050 µs. CPUs were pinned but remained available
to other OS work. The offered traffic does not establish maximum throughput.

The application timings exclude NIC, driver and kernel delivery before the
receive call returns. The hardware-timestamp results above come from a separate
RX-only probe on another receiver; their percentiles cannot be added to these
figures to obtain end-to-end latency. Combined NIC-to-book latency remains
unmeasured. DPDK/kernel-bypass processing is planned.

## Explore or reproduce

- [Build and run](docs/usage.md): local checks and separate-host experiments.
- [Experiment reports](docs/experiments/README.md): hardware, methods, outcomes
  and reviewed JSON/CSV result tables.
- [Workload recipes](simulator/play_files/README.md): generate the four profiles.
- [Project structure](docs/structure.md) and [processor design](processors/README.md).
- [Protocol scope](docs/market-data.md): the implemented PITCH subset.

Generated workloads and raw captures are not distributed; the workloads can be
regenerated from the supplied recipes.

## License

[MIT](LICENSE). SipHash-derived code includes the
[upstream CC0 dedication](docs/licenses/SipHash-CC0.txt).
