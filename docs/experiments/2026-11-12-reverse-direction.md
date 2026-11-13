# Reverse direction: shared EPYC receiver

12 November 2026. Ryzen sender → shared EPYC receiver, with the X550 remaining
in its active-backup bond. RX 2 / L2 3 / observer 15 are on NUMA node 0 with
the NIC. Startup on CPU 3 promotes local first touch without strict memory
binding. C++ was rebuilt with GCC 12.2 O3 on the receiver. Rust used the same
1.99.1 release binary from the Ryzen system. [Hardware/methods](README.md).

One attempt per implementation/profile gave **2/8 valid L2 trials**. Only P2
completed; achieved payload rate was about 4.8004 Mbps. Both final books matched.

| Profile | Implementation | Status | Samples | Valid p99.9 ns |
| --- | --- | --- | ---: | ---: |
| P1 | C++ | incomplete | 71,560 | excluded |
| P1 | Rust | incomplete | 71,432 | excluded |
| P2 | C++ | valid | 1,000,000 | 13170 |
| P2 | Rust | valid | 1,000,000 | 92513 |
| P3 | C++ | incomplete | 71,440 | excluded |
| P3 | Rust | incomplete | 72,832 | excluded |
| P4 | C++ | incomplete | 22,424 | excluded |
| P4 | Rust | incomplete | 30,976 | excluded |

Each incomplete trial stopped at its first bounded eight-message gap and queued
the exact range. Socket and ring drops were zero. The sender continued its
file, so the first gap does not count all subsequent loss. Partial latency
records remain in [JSON](2026-11-12-reverse-direction-results.json) and
[CSV](2026-11-12-reverse-direction-results.csv), excluded from successful
summaries. Single successful observations do not support a language ranking.

## Full-file software timestamp captures

An independent RX-only probe recorded every delivered packet and software RX
stamp. Expected packets were 125,024 in every profile. Transmit queue counters
were read immediately before and after each replay:

| Profile | Received datagrams | Missing | Sender fq_codel drop increase |
| --- | ---: | ---: | ---: |
| P1 | 124,458 | 566 | 566 |
| P2 | 125,024 | 0 | 0 |
| P3 | 124,456 | 568 | 568 |
| P4 | 122,494 | 2,530 | 2,530 |

The exact matching deficits identify sender-queue dropping in these captures.
The reason for backpressure at these modest offered rates remains unresolved.
Read-only inspection found negotiated Ethernet pause enabled, which does not
by itself prove the cause. Faster replay rates stretched below plan.
Removing a receiver bond member would not address these observed sender drops.

The valid P2 software capture measured kernel-RX-to-userspace p50 18.401 µs,
p99 54.071 µs, p99.9 2.300 ms and maximum 20.540 ms. Its REALTIME/MONOTONIC
read brackets consistently intersected within 40 ns. It is a separate probe
workload, not a component that can be added to independently measured L2
percentiles.

Hardware, OS, NIC and C++ compiler changed together on reversal. This cannot
isolate one cause for differences from the original direction. Subsequent
[hardware timestamp captures](2026-11-12-hardware-rx.md) used only P2 and
established full hardware coverage without removing either bonded port.
