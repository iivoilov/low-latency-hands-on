# CPU affinity, NUMA and L3 placement

12 November 2026. GCC 14.2 O3 on the Ryzen receiver, EPYC sender, P1 seed 43.
Five interleaved repeats of four placements gave **20/20 valid million-update
runs**, matching final books with zero detected loss. Observer CPU 15 sampled
thread placement every 250 ms. [Common methods](README.md).

The receiver has one NUMA node and two 32 MiB L3 domains: CPUs `0-7,16-23` and
`8-15,24-31`. SMT pairs are `i,i+16`; selected RX/L2 pins use different physical
cores. This isolates placement choices on one host, not different NUMA nodes.

All times below are nanoseconds; medians/ranges describe per-run percentiles.

| Placement | Median p50 | Median p99.9 | p99.9 range | p99.9 MAD |
| --- | ---: | ---: | ---: | ---: |
| OS placement | 320 | 1000 | 590–1180 | 180 |
| RX 2 / L2 3 | 300 | 600 | 570–620 | 20 |
| RX 10 / L2 11 | 330 | 620 | 610–660 | 10 |
| RX 2 / L2 11 | 710 | 1330 | 1290–1350 | 10 |

RX 2 / L2 3 reduced observed p99.9 variability relative to the unpinned control.
Cross-L3 placement was repeatable but slower. Median per-run mean queue wait
was approximately 79 ns on CPUs 2/3 and 326 ns across L3. This is consistent
with a more expensive producer/consumer handoff, but no hardware counters were
collected to isolate its mechanism. The selected later baseline is RX 2 / L2 3.

Pinning does not remove outliers: the largest valid latency was 80,001 ns.
All sampled pinned placements matched requests. A first wakeup after pinning
can count as an RX migration; sampled scheduler counters are not complete traces.

The unpinned RX thread used observer CPU 15 in one round. Contention with the
observer could contribute to control jitter. IRQ placement, frequency scaling,
other workloads, SMT activity and startup state remain possible influences.
No IRQ, governor or NIC tuning was applied. Five repeats do not establish
confidence intervals or a universal placement rule.

[JSON summaries and every trial](2026-11-12-cpu-affinity-results.json).
[Placement commands](../../benchmark/README.md#cpu-placement).
