# Repeated experiments on separate hosts

This guide uses your own SSH configuration and interface addresses. It does not
install packages, change NIC settings or reserve CPUs. [Usage](../docs/usage.md)
covers local playback and the [experiment index](../docs/experiments/README.md)
describes the measured configurations.

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

Generate the same [seed-43 workload profiles](../simulator/play_files/README.md)
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

The default [baseline](../configs/baseline.json) is RX 2 / L2 3 / observer 15.
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
