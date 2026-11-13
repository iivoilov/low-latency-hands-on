# Experiments and measurement methods

Measurements were collected on 12 November 2026. The names below are hardware
labels, not host names. [Machine-readable hardware](../../configs/hardware.json)
and [profile parameters/checksums](profiles.json) accompany the reports.

| Label | CPU and OS | Network and role |
| --- | --- | --- |
| Ryzen | Ryzen 9 9950X, 16 cores / 32 threads, Debian 13, Linux 6.12.116 | r8169, 1 Gbps; original receiver, reversed sender |
| EPYC | 2 × EPYC 7742, 128 cores / 256 threads, Linux 6.1.0-54 | Dual Intel X550, active-backup bond, 1 Gbps; original sender, reversed receiver |

The Ryzen system has one NUMA node and two 32 MiB L3 domains. The EPYC system
has two NUMA nodes and runs other server workloads; the tested NIC and RX/L2
cores are on node 0. Placement pins are not exclusive CPU reservations.

## Reports

1. [C++ compiler/optimization comparison](2026-11-12-cpp-os.md): initial 100k and
   1m-update controls, one retained loss event, four profiles with a new seed.
2. [CPU affinity and cache topology](2026-11-12-cpu-affinity.md): five interleaved
   repeats per placement, same-L3 and cross-L3 handoff.
3. [Pinned C++ versus Rust](2026-11-12-rust-vs-cpp.md): four profiles, three repeats,
   matching L2 semantics, implementation differences and outliers.
4. [Reversed traffic direction](2026-11-12-reverse-direction.md): a shared EPYC
   receiver and matching sender-queue drop/missing-packet counts.
5. [X550 hardware RX timestamps](2026-11-12-hardware-rx.md): intact bond, full
   timestamp coverage, receive-path decomposition and clock-correlation limits.

## Workload and timing contract

The synthetic BYX/EDGA equities PITCH subset initializes three symbols with ten
levels on each side and three orders per level, then emits one million measured
updates. New dead-order replacements maintain a bounded book. Default packets
carry up to eight market events. All profiles use 20 Mbps peaks; background,
burst duration and period vary. Rates are decimal PITCH UDP payload bits,
excluding network overhead. All four seed-43 files carry 125,024 datagrams.
See [recipes](../../simulator/play_files/README.md) and [protocol scope](../market-data.md).

The L2 processors use MONOTONIC elapsed time from immediately after `recvmsg`
return to each completed update. This includes ring wait, decoding, allocation,
clock reads, scheduling and earlier messages in the same packet. Initialization
and controls are excluded. It excludes NIC/driver/kernel delivery before RX
returns. It is not thread CPU time or a wire-to-L2 measurement.

The hardware probe is a separate RX-only workload measuring NIC timestamp to
userspace, with a kernel software timestamp as an intermediate point. Both
clock domains are correlated on the receiver; sender clock synchronization is
unnecessary. Do not add or subtract independently calculated percentiles from
different intervals or experiments.

Percentiles use nearest rank without interpolation. Summaries are medians/ranges
of per-run percentiles, not pooled percentiles. Messages in a packet share an
arrival timestamp; one million updates are not independent wire observations.
A valid L2 run requires one million samples, no loss, verified placement where
requested and exact final-book agreement. Failed runs are kept and excluded
from successful latency summaries. No automatic retries conceal failures.

## Result data and limits

Each report links compact reviewed JSON/CSV tables. Numerical per-trial results,
workload checksums, failure counts and historical build fingerprints are retained;
connection details and process/host inventories are omitted. Earlier seed-42
compiler batches provide aggregate results plus the failed-run gap, rather than
all raw logs. Raw packet captures and operational logs are not distributed.

Recorded fingerprints identify the historical measured builds. New documentation
and preparation tools are packaging additions; they are not claims that the
historical binaries were rebuilt from this public tree. The processor/simulator
source is provided for reproduction, and regenerated profile hashes can be
checked against `profiles.json`.

These observations cover small synthetic books and limited repetitions.
Containers, toolchains, CPU topology, scheduling and network direction all matter.
No confidence intervals or universal language/hardware rankings are claimed.
DPDK, exact physical-wire calibration and paired wire-to-L2 measurements remain
future work. Run your own tests with [the usage guide](../usage.md).
