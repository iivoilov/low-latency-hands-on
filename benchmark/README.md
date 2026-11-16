# Repeated experiments on separate hosts

This guide uses your own SSH configuration and interface addresses. It does not
install packages, change NIC settings or reserve CPUs. [Usage](../docs/usage.md)
covers local playback and the [experiment index](../docs/experiments/README.md)
describes the measured configurations.

The selected C++ baseline uses SipHash-2-4 with a secret per-book key and no
bucket-length rejection. Both processors default to a million live orders and
include application-level validation/resource limits. See the
[security description](../processors/README.md#input-validation-and-resource-protection)
and [native hardening comparison](../docs/experiments/2026-11-13-siphash.md).
Historical results retain their measured hash, capacity and validation behavior.

Performance changes must retain a matching pre-change source/build and timing
control. Compare each independent change separately and together, using identical
workloads, limits, timing boundaries and placement. Shuffle variant order across
repetitions, retain every attempt, and report regressions as well as gains.
Correctness tests alone do not establish a performance benefit. Label local WSL2
measurements separately from qualified separate-host receiver experiments.

`run_local_changes.py --config CONFIG.json --output FRESH_DIRECTORY` provides a
local loopback comparison of frozen binaries using the existing pinning launcher.
The configuration supplies `variants` (each name maps to `language`, `change`,
`binary` and `source_manifest`), `sender`, `oracle_file`, `profile`, `repeats`,
`seed`, and explicit `rx_cpu`, `l2_cpu`, `sender_cpu`, `observer_cpu` roles.
Each language needs a variant with `change: "baseline"`. A source manifest maps
relative file paths to SHA-256 hashes, with the frozen source under sibling `src/`.
The oracle file maps profile names to the frozen PCAP path/hash, generator
manifest, independent final L1 and decision/trade counts. All paths are local;
the tool performs no deployment or host tuning. It verifies source/input hashes,
exact L1/L2 state and counts, sample/warmup counts, loss, placement and offered
rate. Raw reports and incremental JSON/CSV summaries preserve failed attempts;
there are no automatic retries. Summary changes are relative to that language's
baseline; percentile rows are medians of valid per-run percentiles.

The 2026-11-16 local ring/quantity comparison completed 24/24 valid trials:
four source variants (baseline, ring caching, in-place quantities, both), C++ and
Rust, three shuffled repetitions. It reuses S1's 503-symbol, seed-44 workload:
30,180 initialization adds and one million measured updates at 8/20 Mbps,
10/100 ms bursts. Every trial matches the independent wire oracle's final L1/L2
and decision/trade counts, with 128,773 processed packets and zero loss.
The terminal heartbeat is outside the sample window. All recorded RX/L1 masks
are single-CPU and migration deltas are zero.

This is Intel i9-13900K WSL2 loopback with local RX 2, book/L1 4, sender 6 and
observer 8, startup 4. These are distinct guest-reported physical cores, without
physical-host isolation. The ryzen placement configuration is not reused.
GCC 12.2 uses Release `-O3 -DNDEBUG`; Rust 1.80.1 uses release opt-level 3,
16 codegen units, no LTO. No packages, remote deployments or host tuning change.
Timing remains `CLOCK_MONOTONIC` after `recvmsg` through each message's L1
decision, including queue wait. This excludes NIC/kernel work before return.
Only S1 is measured here; this is not the separate-host baseline qualification.

| Variant | C++ p50 / p99 / p99.9 µs | Rust p50 / p99 / p99.9 µs |
| --- | ---: | ---: |
| Baseline | 4.756 / 22.083 / 121.202 | 4.520 / 24.057 / 113.453 |
| Ring caching | 4.754 / 24.133 / 154.769 | 4.491 / 26.205 / 130.617 |
| In-place quantities | 4.723 / 22.662 / 136.566 | 4.502 / 24.452 / 129.679 |
| Both | 4.772 / 23.456 / 133.485 | 4.498 / 24.961 / 155.079 |

The observed median p50 changes are below 0.7%, including a 0.34% regression for
C++ with both changes. Every changed variant has a higher median p99 and p99.9
than its matching control. These tail regressions are retained; their cause is
unassigned. Three noisy local repetitions do not demonstrate an overall speedup.
The quantity path applies to 24,325 same-price positive modifications, only
2.4325% of measured updates. Ring high-watermarks range from 8 to 50 of 4096 slots;
publication still reads the consumer position for depth reporting. Those facts
limit extrapolation to other workloads. Actual sender rates are 9.18551–9.19581
Mbps. No failed timing trial or automatic retry was excluded.

Portable [CSV](../docs/experiments/2026-11-16-ring-quantity-local-results.csv) and
[JSON](../docs/experiments/2026-11-16-ring-quantity-local-results.json) retain all
percentiles, per-run results, source/binary/input identities and limitations.
Raw source archives, binaries, build flags/logs, the harness/configuration,
oracle checks, affinity audit and reports are under
`analysis/ring-quantity-2026-11-16-local/`; the original builds remain intact.
Re-run with a fresh destination and the saved configuration, for example:

```sh
python3 benchmark/run_local_changes.py \
  --config analysis/ring-quantity-2026-11-16-local/config.json \
  --output analysis/ring-quantity-2026-11-16-local/repeat-001
python3 -m unittest discover -s benchmark -p 'test_local_changes.py'
```

Validation passed 11 offline and four network CTests, 20 Rust native tests, the
shared Rust CLI/multicast/GRP/spin suites, and six benchmark-tool tests. Each of
the four isolated source variants also passed C++ processor/L1 and Rust native
checks before timing. The initial sandboxed network invocation denied socket
creation; all four network CTests passed with local socket access. Builds and
tests did not run concurrently with timing trials. An optimized GCC warning in
the L1 test packet builder was removed by sizing its buffer before copying.

## Native ring and quantity comparison, 2026-11-16

The selected epyc-to-ryzen comparison completed **54/54 valid new trials**:
ring caching, in-place quantities and both together, across GCC O3, Clang O3 and
Rust release, both frozen S&P 500 profiles and three shuffled repetitions.
At the user's request, the existing 18 valid pre-change trials from
[the earlier S&P 500 matrix](../docs/experiments/2026-11-16-sp500.md) supply the
baseline; they were not rerun. All pre-change C++/Rust runtime source hashes
match that baseline. The pinning launcher, Rust build helper and Cargo manifest/
lock also match. The only CMake difference adds the `monitoring_replay` test;
processor build definitions and flags are unchanged.

Ryzen is the Ryzen 9 9950X receiver with the existing 1 Gbps r8169 interface;
epyc is the EPYC 7742 sender with its active-backup X550 bond on `eno1`.
Both run Linux 6.12.117+deb13-amd64. GCC is 14.2.0, Clang 19.1.7 and Rust
1.99.1 / LLVM 23.1.1. C++ uses `-O3 -DNDEBUG`, no LTO or native target flags;
Rust uses opt-level 3, no LTO and 16 codegen units. RX 2, book/L1 3, observer 15
and startup 3 preserve the selected receiver placement. All 54 affinity records
show sampled RX/L1 CPUs 2/3, single-CPU masks and zero recorded migration deltas.
No packages, host/NIC settings, governors, BIOS settings or services changed.
Three fresh source/build roots on each host preserve all older deployments.

Each trial replays the same seed-44, 503-symbol file, with 30,180 initialization
adds and 1,000,000 measured updates. S1 uses 8/20 Mbps and 10/100 ms bursts;
S2 uses 8/20 Mbps and 25/250 ms bursts. Both retain the fixed background and
9.2 Mbps cycle average. No new workloads were generated. Monitoring and recovery
are disabled. `CLOCK_MONOTONIC` starts after successful `recvmsg` return and
ends after each sampled L1 emit/no-update decision, including ring wait and
earlier messages in the packet. This is not NIC/kernel, physical-wire or
cross-host one-way latency.

Every new trial has exact independent-wire-oracle agreement for final L1/L2,
decision/emission/suppression counts and retained trades. Each processes 128,773
packets before the terminal heartbeat, with zero kernel/ring loss, missing
messages, sequence/protocol errors or skipped market updates. Actual sender
rates span 9.18450–9.19827 Mbps; cumulative schedule slip spans 15.374–50.201 ms,
with maximum single-send lateness 0.974413 ms. All group qdisc drop deltas are
zero. All 54 planned trials completed on the first attempt, without retries or
excluded runs. Source files and receiver binary hashes match postflight checks.

Values below are **p50 / p99 / p99.9 in µs**, medians of three per-run
nearest-rank percentiles. Profiles remain separate; samples are not pooled.

| Profile / build | Existing baseline | Ring caching | In-place quantities | Both |
| --- | ---: | ---: | ---: | ---: |
| S1 GCC O3 | 1.490 / 3.220 / 3.920 | 1.500 / 3.240 / 3.990 | 1.500 / 3.230 / 3.990 | 1.480 / 3.220 / 3.950 |
| S1 Clang O3 | 1.350 / 2.910 / 3.570 | 1.350 / 2.900 / 3.590 | 1.360 / 2.950 / 3.710 | 1.350 / 2.910 / 3.570 |
| S1 Rust release | 1.510 / 3.230 / 4.520 | 1.510 / 3.250 / 4.510 | 1.470 / 3.180 / 4.420 | 1.520 / 3.250 / 4.560 |
| S2 GCC O3 | 1.480 / 3.200 / 3.880 | 1.510 / 3.270 / 3.970 | 1.510 / 3.260 / 3.960 | 1.490 / 3.240 / 3.970 |
| S2 Clang O3 | 1.360 / 2.930 / 3.590 | 1.340 / 2.890 / 3.530 | 1.370 / 2.960 / 3.680 | 1.360 / 2.930 / 3.610 |
| S2 Rust release | 1.510 / 3.250 / 4.510 | 1.500 / 3.220 / 4.470 | 1.480 / 3.160 / 4.330 | 1.520 / 3.260 / 4.480 |

Ring-only has no consistent general benefit: GCC median p50 is 0.7–2.0% higher,
Clang is unchanged to 1.5% lower, and Rust is unchanged to 0.7% lower. Quantity-only
improves Rust median p50 by 2.0–2.6% and p99.9 by 2.2–4.0%, while C++ medians
are slightly slower. Combining both leaves C++ median p50 within ±0.7% and
raises Rust p50 by 0.7%; it does not retain the Rust quantity-only gain.
The full distribution ranges and measured regressions remain in the results.

These are small, implementation-specific changes at the selected offered load,
not a throughput-capacity result. Only 24,325 updates (2.4325%) take the new
same-price positive modification path; execution-at-price semantics are covered
by native tests but not exercised by this synthetic workload. Ring high-watermarks
range from 2 to 15 of 4096 slots, and publication retains the shared consumer-index
read for depth reporting. The baseline was measured earlier the same day and
was not interleaved with these changed variants. Hash keys remain independently
generated per process, and affinity does not isolate CPUs from OS work. These
limits matter when interpreting differences of tens of nanoseconds. No cause is
assigned to the non-additive results or individual tail variations.

Native validation passed 15/15 CTests under each C++ compiler for each variant
(90 checks), 20/20 Rust native tests per variant (60 tests), all three shared
Rust CLI/multicast/GRP/spin suites, and 4/4 sender CTests per variant (12 checks).
The local remote-runner unit suite passed 13 tests. All builds and tests completed
before timing. This comparison changed no processor source beyond the two
already implemented optimizations.

Portable [CSV](../docs/experiments/2026-11-16-ring-quantity-ryzen-results.csv) and
[JSON](../docs/experiments/2026-11-16-ring-quantity-ryzen-results.json) include
all 54 new and 18 reused baseline measurements, per-profile/build changes,
percentile ranges, source/binary/input identities and placement/state audits.
Raw source archives, deployment/build/test scripts and logs, commands, all
receiver reports, affinity records and host/network inventories remain under
`analysis/ring-quantity-2026-11-16-ryzen/`. `baseline-audit.json` records source
equivalence; `remote-roots.json`, `run_matrix.py`, `collect_postflight.py` and
`summarize.py` retain the execution and analysis. Use a fresh experiment directory
for a later run; preserve this complete attempt and the earlier baseline.

For the local, decoder-only comparison, build `pitch_decode_bench` and use
`--fixtures processors/tests/equities_messages.tsv` or `--pcap PATH`.
`--messages`, `--warmup`, `--repeats` and `--seed` control batch size and paired
repetitions. It compares `decode_pitch_message` with the experimental packed
version and reports batch-average ns/message. It does not measure RX-to-L1.
The [packed decoder report](../docs/experiments/2026-11-14-packed-decoder.md)
contains commands, validation, all measured sessions and limitations.

Add `--decoders original,packed,zero-copy` to compare all three versions, or
select any two of those names. `packed` copies each wire layout to a local
object; `zero-copy` reads a packed reference directly from writable input bytes.
All versions return the same normalized message. See the
[zero-copy results](../docs/experiments/2026-11-14-zero-copy-decoder.md).

For the per-layout and ordering investigation, use
`python3 benchmark/run_decode_breakdown.py --pcap PATH --output FRESH_DIRECTORY`.
The same binary accepts repeatable `--type N`, `--length N`, `--copies N` and
`--order input|grouped|shuffled` controls. Backing wire bytes/relative offsets
remain fixed; ordering/repetition operates on message descriptors. The
[breakdown report](../docs/experiments/2026-11-14-decoder-breakdown.md) includes
all layouts, PCAP types, ordering controls and a separate scalar-load diagnostic.
Pass `--include-zero-copy` to the breakdown runner for the three-way matrix.

## Current workload and timing

Current processors report `rx_to_l1_decision`, ending after each message's
L1 emit/no-update decision. Historical `rx_to_l2` reports remain separate;
the runner records the metric/boundary and never merges them in a summary.
Routine comparisons use the frozen 503-stock, seed-44 S1/S2 workloads, GCC O3,
Clang O3 and Rust release, with three repetitions per profile. Pass the manifest's
30,180 initialization count; the raw default 180 is for three-stock fixtures.
See [the workload report](../docs/experiments/2026-11-16-sp500.md). The runner
checks final L2; the published matrices additionally checked final L1 and counts
against an independent wire oracle.

The example command below retains a historical three-stock profile to keep
reproduction possible without the original vendor snapshot. Use your frozen
S1/S2 paths for the routine comparison. Vendor data is read only during preparation.

## Prepare identical source trees

Copy this source tree to both hosts, or clone the same published revision there.
On each host, fingerprint the source and then build a C++ Release variant:

```sh
python3 benchmark/prepare_manifest.py
cmake -S . -B build/gcc-o3 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++ '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG' \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/gcc-o3 -j 4
ctest --test-dir build/gcc-o3 --output-on-failure
```

The helper creates ignored `.deploy/source-manifest.json` from source, test,
baseline and tool files. The runner checks the snapshot against actual sources
on both hosts. Regenerate it and rebuild after code changes; do not edit code
between the fingerprint, build and run. The manifest is a source consistency
check, not proof that an arbitrary binary was built from those sources.

For other C++ variants use `gcc-o2`, `clang-o2`, `clang-o3`, compiler `g++` or
`clang++`, and the corresponding `-O2` or `-O3` flag. Keep each build in its own
directory and export compile commands. For Rust, run on the receiving host:

```sh
python3 benchmark/build_rust.py
```

That creates `build/rust-o3/processor_rust` and build metadata. The runner
validates the Rust source digest against those recorded build inputs.

Generate the same [frozen workload profiles](../simulator/play_files/README.md)
on the orchestration machine and sender. The runner checks PCAP SHA-256 before
sending. For a LAN test, verify multicast reaches the receiving interface and
that both hosts allow the selected group/UDP port.

## Run a comparison

The following values are placeholders. Replace SSH names, absolute checkout
paths, IP addresses and CPU IDs with your own configuration:

```sh
RX_HOST=receiver.example
TX_HOST=sender.example
RX_ROOT=/path/to/receiver/checkout
TX_ROOT=/path/to/sender/checkout
RX_IP=192.0.2.10
TX_IP=192.0.2.20
python3 benchmark/run_remote_cpp.py \
  --receiver-host "$RX_HOST" --sender-host "$TX_HOST" \
  --receiver-root "$RX_ROOT" --sender-root "$TX_ROOT" \
  --receiver-interface "$RX_IP" --sender-interface "$TX_IP" \
  --sender-build gcc-o3 --builds gcc-o3 rust-o3 --repeats 3 \
  --rx-cpu 2 --l2-cpu 3 --observer-cpu 15 --continue-on-invalid \
  --profile simulator/play_files/pitch-v2-1m-base8m-peak20m-b10ms-e100ms_sym3_seed43.pcap \
  --output analysis/comparison-p1
```

The orchestration machine needs the local profile PCAP and manifest. Both remote
hosts need Python 3 and `timeout`. Build directory names identify executables;
the Rust helper records `binary_name`, while CMake builds use `processor_cpp`.
Use a fresh output basename for every invocation: the remote receiver stores
its run files under `analysis/BASENAME`, and existing directories are refused.

The receiver starts first and reports an ephemeral UDP port. The external
launcher applies CPU placement before forwarding READY; only then does the
sender replay with `--spin`. Three rounds shuffle variant order reproducibly.
Timeouts scale with the manifest. Every attempted trial retains logs, hashes,
sender pacing, CPU observations and receiver reports, including failed trials.

Validity requires a complete sample target, no loss, correct CPU masks and an
exact final-book match to the play-file reference. `--continue-on-invalid`
continues to the next independent trial without retrying a failure; the batch
returns nonzero if any trial was invalid. Summaries use only valid complete
runs and report medians of per-run percentiles, not pooled percentiles.

## CPU placement

The default [baseline](../configs/baseline.json) is RX 2 / L2 3 / observer 15,
scoped to the neutral `ryzen` receiver label. Configure your receiver alias in
`receiver_host`, select a matching `--affinity-config`, explicitly supply all
three CPU roles, or use `--unpinned`. A different receiver never silently
inherits the Ryzen CPU/observer assignment.
Choose IDs from your allowed CPU mask and check NUMA/cache/SMT topology first.
`--unpinned` explicitly leaves RX/L2 to the OS. `--startup-cpu N` constrains
startup first-touch allocation before per-thread placement; it is not strict
memory binding. A pinned benchmark still shares the host with other workloads.

The wrapper supports exactly two processor threads: main=L2 and worker=RX.
It samples effective masks, last-CPU IDs and scheduler counters every 250 ms.
These observations are not a full scheduler trace. The initial last-CPU value
can predate pinning when a thread is blocked; first wakeup can add a migration.
An unpinned thread can overlap the observer's CPU. See the
[affinity report](../docs/experiments/2026-11-12-cpu-affinity.md).

## Automated hardware captures

First run the privileged timestamp session on the receiver as described in
[hardware usage](../docs/usage.md#hardware-receive-timestamps). Check its active
port's PHC and select two valid CPU IDs. Then, with the variables above:

```sh
python3 benchmark/run_wire_capture.py \
  --receiver-host "$RX_HOST" --sender-host "$TX_HOST" \
  --receiver-root "$RX_ROOT" --sender-root "$TX_ROOT" \
  --receiver-interface "$RX_IP" --sender-interface "$TX_IP" \
  --receiver-build gcc-o3 --sender-build gcc-o3 \
  --bond bond0 --phc /dev/ptp0 --rx-cpu 2 --calibration-cpu 15 \
  --profile simulator/play_files/pitch-v2-1m-base4m-peak20m-b5ms-e100ms_sym3_seed43.pcap \
  --repeats 3 --output analysis/hardware-p2
```

The automated capture runner expects a bond; the raw probe can also run on an
unbonded interface by omitting `--bond`. Hardware mode checks the active PHC and
RX filter, retains raw packet and clock CSVs, and correlates clocks afterward.
`--software-only` replaces `--phc` for software-only capture. Stop the privileged
session afterward and verify restoration. Inspect [the hardware report](../docs/experiments/2026-11-12-hardware-rx.md)
for measurement limits and interpretation.
