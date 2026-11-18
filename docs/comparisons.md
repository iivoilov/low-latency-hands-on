# Reference systems: what their benchmarks measure

Reviewed 19 November 2026. These are primary project sources, not reruns.
No defensible direct latency comparison with this receiver was found among the
references below. Partial comparability means related processing or architecture;
it does not justify a speed ratio, a language ranking or a shared latency chart.

Our reference boundary is successful `recvmsg` return to each message's L1
emit/no-update decision, including ring wait, decode, L2/trades/metadata and
allocations. It excludes earlier NIC/kernel delivery. The
[latest native study](../benchmark/README.md#native-ring-and-quantity-comparison-2026-11-16)
uses a Ryzen 9 9950X, Linux sockets, verified process affinity and synthetic
503-stock PITCH traffic. Statistics are nearest-rank p50/p99/p99.9 in ns,
summarized as medians of three per-run percentiles; figures convert ns to µs.

## LMAX Disruptor: handoff and pipeline costs

- **Operation:** the historical Disruptor 1.x comparison uses an in-process
  three-stage event pipeline, injecting 50 million events with a 1 µs wait
  between injections, below saturation.
- **Boundary:** the wiki describes injection through pipeline processing and
  characterizes its 52 ns mean as per-hop latency. The exact legacy timestamp
  sites and normalization are not established by the wiki alone; do not treat
  52 ns as a complete three-stage processing time.
- **Environment:** Core i7-2720QM at 2.2 GHz, Ubuntu 11.04, Java 1.6.0_25
  64-bit; invariant TSC, no CPU binding.
- **Statistics:** reported mean 52 ns, 99% below 128 ns and 99.99% below
  8,192 ns. These are historical results, not a current release guarantee.
- **Comparability:** not numerically comparable. Relevant to bounded handoff,
  backpressure and wait strategies; no PITCH decoding or order-book work.

Primary sources: [project](https://github.com/LMAX-Exchange/disruptor),
[historical performance methodology and table](https://github.com/LMAX-Exchange/disruptor/wiki/Performance-Results).

## Aeron benchmarks: messaging round trips

- **Operation:** the echo scenario sends a payload over Aeron UDP and echoes
  the reassembled message back. Archive and cluster scenarios add different work.
- **Boundary:** the harness sends messages at a configured rate and times
  responses at the originating client. That is a round trip. The linked overview
  does not pin exact timestamp instructions relative to publication acceptance,
  so no instruction-level boundary or one-way estimate is asserted here.
- **Environment:** operator-configured client/server hosts, JVMs, CPU placement,
  channels, payload size, rate and burst size. The repository's setup example
  specifies JDK 17+; it is a harness rather than one fixed hardware result.
- **Statistics:** latency histograms per run, with HDR result aggregation.
  Inspect a specific run's histogram units, scenario and configuration before
  quoting its percentiles. No standalone numeric result is selected here.
- **Comparability:** not numerically comparable. Useful for controlled offered
  load, reproducible messaging tests and explicit transport/archival semantics.

Primary sources: [benchmark overview](https://github.com/aeron-io/benchmarks#readme),
[scenario guide](https://github.com/aeron-io/benchmarks/blob/master/scripts/aeron/README.md).

## DPDK: packet forwarding capacity

- **Operation:** one concrete archived example is the Broadcom P225p 2×25G
  RFC2544 zero-frame-loss test, using `testpmd` and IXIA traffic, 256 IP flows
  per port. At 64-byte frames the report lists 68.11 Mpps on one core.
- **Boundary:** traffic-generator transmit/receive accounting across a forwarding
  system. This table is a throughput result; no per-packet latency timestamp
  boundary or percentile is supplied.
- **Environment:** Dell R740, Xeon Gold 6154 at 3 GHz, 128 GB RAM, Broadcom
  P225p, RHEL 7.6 / kernel 3.10.0-957, DPDK 22.07. It uses core isolation,
  hugepages and power/BIOS changes absent from our socket baseline.
- **Statistics:** millions of forwarded packets per second at zero frame loss,
  not order-book updates/s or application latency in ns.
- **Comparability:** not comparable numerically. Relevant to a future receive-path
  experiment; these forwarding results predict no PITCH processing percentile.

Primary sources: [DPDK performance report index](https://core.dpdk.org/perf-reports/),
[Broadcom report, section 1 / tables 1–3](https://fast.dpdk.org/doc/perf/DPDK_22_07_Broadcom_NIC_performance_report.pdf).

## CppTrader: a closer market-data reference with a different statistic

- **Operation:** NASDAQ ITCH parsing and an order-book market manager using the
  `01302017.NASDAQ_ITCH50` file, 283,238,832 messages. The project supplies
  separate parser, manager and optimized-manager benchmarks.
- **Boundary:** the manager source timestamps before its input-read loop and
  after the loop completes. It includes file/stdin reads, parsing and callbacks.
  Reported message time divides the total by message count; market-update time
  uses a different callback count.
- **Environment:** i7-4790K at approximately 4 GHz, 32 GiB RAM, Windows 8
  Enterprise 64-bit, release build, hyperthreading enabled. The README does not
  specify the compiler version/flags for those results.
- **Statistics:** parser 24 ns/message; ordinary manager 309 ns/message;
  optimized manager 120 ns/message. These are batch-average costs, not p50,
  p99 or p99.9 observations, and not socket-to-L1 elapsed times.
- **Comparability:** partially comparable in functionality only. Different
  protocol, input, state/notification semantics, platform and timing method
  prevent a direct ranking. Its preallocation experiments are useful design references.

Primary sources: [performance report](https://github.com/chronoxor/CppTrader#performance),
[manager timestamp loop and arithmetic](https://github.com/chronoxor/CppTrader/blob/main/performance/market_manager.cpp).

## Limit-Order-Book: matching work is a different task

- **Operation:** five million generated requests, with market, limit, stop and
  stop-limit orders; 11,000 initial orders.
- **Boundary:** request entry to completion of all resulting matching-engine
  operations, according to the author's measurement description.
- **Environment:** Intel i5-12450H at 2 GHz, C++. The result section does not
  establish OS, compiler/flags or clock details.
- **Statistics:** reported mean 713 ns/request and a latency histogram;
  no matching p99/p99.9 table is selected.
- **Comparability:** partially comparable data-structure work, not equivalent
  semantics. A matching engine creates executions; our receiver applies events
  already produced by an exchange. No multicast receive or packet-queue boundary.

Primary source: [author's workload and latency report](https://github.com/brprojects/Limit-Order-Book#testing--performance).

## What would make a direct comparison defensible?

Run both implementations on the same receiver and identical packet bytes,
with matched protocol coverage, state limits, trade corrections, L1 decisions,
loss policy, warmup and offered-load schedule. Use the same timestamp sites and
clock, record per-run percentiles and achieved rate, verify final state independently,
and preserve failed runs. Report CPU placement, toolchains, source/binary/input
hashes and host activity. Neither a reciprocal throughput calculation nor a
round-trip divided by two substitutes for that experiment.

External repositories and their default branches can change. The historical
numbers above belong to the cited reports, not necessarily today's source.
