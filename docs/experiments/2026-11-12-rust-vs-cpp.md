# Pinned C++ versus Rust

12 November 2026. EPYC sender → Ryzen receiver, four seed-43 profiles, three
repeats per implementation/profile. RX CPU 2 / L2 CPU 3 share L3; observer CPU
15 monitors placement. All **24/24 trials** completed one million samples,
matched final books and reported zero detected loss. [Methods/hardware](README.md).

C++: GCC 14.2 O3, `std::unordered_map` plus `std::map`. Rust: 1.99.1,
LLVM 23.1.1, release opt-level 3, LTO off, 16 codegen units, `HashMap` with
RandomState plus `BTreeMap`. Both use ordinary Linux UDP sockets, a bounded
packet ring, gap notifications, the same warmup and the same clock boundaries.
Book allocation is timed; JSON serialization/output is not.

Median of three per-run percentiles, nanoseconds:

| Profile | C++ p50 | Rust p50 | C++ p99.9 | Rust p99.9 |
| --- | ---: | ---: | ---: | ---: |
| P1 | 300 | 320 | 570 | 620 |
| P2 | 310 | 320 | 610 | 610 |
| P3 | 310 | 320 | 600 | 630 |
| P4 | 300 | 320 | 610 | 630 |

The implementations were close on this small synthetic book. The experiment
compares concrete containers and toolchains as well as language. Different
hashing/allocation/layout and a limited sample of runs prevent language-wide
conclusions. Affinity had a larger effect in the separate placement study.

Tail summaries do not replace maxima: a valid Rust P3 trial had a **151,691 ns**
maximum while its p99.9 was 620 ns. High observations are retained. Reported
p99.9 values are elapsed time after userspace receive, not wire-to-book latency.

[JSON: summaries and every trial](2026-11-12-rust-vs-cpp-results.json) ·
[CSV](2026-11-12-rust-vs-cpp-results.csv) ·
[Build and run](../../benchmark/README.md).
