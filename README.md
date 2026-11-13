# Low-latency hands-on

Hands-on market-data latency experiments using synthetic PITCH multicast,
C++ and Rust L2 order-book processors, and NIC hardware receive timestamps.
The project includes feed preparation/replay, repeatable workloads, tests,
benchmark tools and results. Current processors use Linux UDP sockets; DPDK is planned.

[Build and run](docs/usage.md) · [Project structure](docs/structure.md) ·
[Methods and hardware](docs/experiments/README.md) · [Protocol scope](docs/market-data.md)

## Main results

### C++ and Rust: userspace RX to L2 update

Ryzen 9 9950X, Debian 13, 1 Gbps; RX CPU 2 / L2 CPU 3.
Each run samples **1,000,000 updates** after initialization. Values below are
medians of three per-run p99.9 measurements, in **nanoseconds**. Peak is 20 Mbps;
rates count PITCH UDP payload bits.

| Profile | Background Mbps | Burst / period | C++ p99.9 ns | Rust p99.9 ns |
| --- | ---: | --- | ---: | ---: |
| P1 | 8 | 10 / 100 ms | 570 | 620 |
| P2 | 4 | 5 / 100 ms | 610 | 610 |
| P3 | 8 | 25 / 250 ms | 600 | 630 |
| P4 | 12 | 20 / 100 ms | 610 | 630 |

All 24 trials completed with matching final books and no detected loss. Timing
starts after `recvmsg` returns and includes ring wait, parsing and L2 updates.
[C++/Rust report and data](docs/experiments/2026-11-12-rust-vs-cpp.md).

**Affinity:** five P1 C++ runs per condition narrowed the p99.9 range from
590–1,180 ns unpinned to 570–620 ns on separate cores sharing L3. Across L3
domains, median p99.9 was 1,330 ns.
[Affinity study](docs/experiments/2026-11-12-cpu-affinity.md).

### X550: NIC timestamp to userspace

A shared dual-EPYC 7742 server received through an intact active-backup bond.
Each P2 capture received **125,024 datagrams**, all with hardware timestamps,
with no detected loss. This separate RX-only probe measures an earlier interval
than the L2 processors.

| Run | NIC → userspace p50 | p99.9 |
| --- | ---: | ---: |
| 1 | 43.716 µs | 0.893 ms |
| 2 | 43.202 µs | 1.547 ms |
| 3 | 43.820 µs | 1.390 ms |

Clock-read bracket half-width reached 2.786 µs. Exact physical-wire calibration
and wire-to-L2 remain unmeasured; original timestamp settings were restored.
[Hardware timestamp report and data](docs/experiments/2026-11-12-hardware-rx.md).

Results cover small synthetic books and limited repetitions. Faster reversed
profiles suffered sender-queue drops; failed trials remain in the
[reversal report](docs/experiments/2026-11-12-reverse-direction.md).

Use the [usage guide](docs/usage.md) and [profile recipes](simulator/play_files/README.md)
to reproduce experiments. Reviewed JSON/CSV tables are included; generated
workloads, raw captures and purchased market data are not distributed.

## License

[MIT](LICENSE).
