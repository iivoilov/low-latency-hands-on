# Build, run and verify

Run commands from the repository root. Linux is the processor/probe target.
Install a C++20 compiler, CMake 3.20+, Python 3.9+, and a Rust toolchain for the Rust
receiver. C++ uses the standard library and Linux/POSIX APIs. Rust dependency
versions are pinned in `Cargo.lock`; initial Cargo use needs registry access or
a checksummed vendor directory. No exchange connection is required.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s benchmark -p 'test_*.py'
python3 benchmark/build_rust.py
CARGO_TARGET_DIR="$PWD/build/rust-tests" cargo test --manifest-path processors/rust/Cargo.toml --locked
```

CTest includes loopback multicast tests. In a socket-restricted environment,
`ctest --test-dir build -LE network --output-on-failure` runs offline cases;
run `-L network` separately where sockets are allowed. Do not treat skipped
network tests as successful networking verification.

Run the shared receiver cases against Rust as well:

```sh
python3 processors/cpp/tests/test_cli.py build/rust-o3/processor_rust build/prepare_feed build/play_feed
python3 processors/cpp/tests/test_cli.py build/rust-o3/processor_rust build/prepare_feed build/play_feed --network
```

The Rust helper records compiler versions, flags, source hashes and logs under
`build/rust-o3/`. It accepts `--vendor-directory` for offline builds. The recorded
Rust 1.99.1 experiment toolchain is distinct from the minimum toolchain needed
to build the project; local compatibility has also been checked with Rust 1.80.1.

## Prepare and replay a workload

Preparation writes a PCAP plus a JSON manifest and never opens a sending socket:

```sh
build/prepare_feed --messages 1000000 --seed 43 \
  --background-mbps 8 --burst-mbps 20 --burst-ms 10 --burst-every-ms 100
```

The default directory is `simulator/play_files/`. The manifest records counts,
timing, parameters and expected final L2. Existing files are never overwritten;
use another `--directory` for a new variant. See [all four recipes](../simulator/play_files/README.md).

Start the receiver in one terminal:

```sh
mkdir -p analysis
build/processor_cpp --interface 127.0.0.1 --warmup-messages 180 \
  --messages 1000000 --output analysis/cpp-loopback.json
```

After its READY line, replay in a second terminal:

```sh
build/play_feed simulator/play_files/pitch-v2-1m-base8m-peak20m-b10ms-e100ms_sym3_seed43.pcap \
  --interface 127.0.0.1 --spin
```

For Rust, substitute `build/rust-o3/processor_rust` and a fresh output filename.
The defaults use multicast group `239.255.0.1`, port `30001`. The play file is
fully loaded before sending. `--spin` busy-waits; delayed sends stretch the
schedule instead of producing catch-up bursts. Inspect achieved Mbps and slip.
`--no-send` validates a PCAP without sending or waiting.

Loopback checks functionality; it is not equivalent to the separate-host results.
For two machines, pass each machine's own interface IPv4 address to its process.
Ensure same-subnet multicast connectivity and matching group/port. The guide's
`192.0.2.x` addresses and `.example` names below are documentation placeholders;
replace them with your own configuration.

## Timing and loss

Processors measure MONOTONIC time from `recvmsg` return to each completed L2
update, including ring wait, parsing and earlier updates in the same packet.
Warmup and protocol controls have no samples. Clock reads and book allocations
are included. Output and serialization happen afterward. This is elapsed time,
not CPU time, and does not include earlier driver/kernel delivery.

Samples share packet arrival conditions; one million messages are not one
million independent wire arrivals. Percentiles use nearest rank. Compare final
books with the manifest, check sample count, and exclude incomplete/lossy runs.

The default stops on loss. `--continuous --on-loss continue` keeps processing,
records drops/gaps and marks incomplete state; it never invents missing orders.
There is no TCP resend, GRP filling or snapshot-recovery implementation.
See [the processor contract](../processors/README.md).

## Separate-host benchmark automation

Use the [benchmark guide](../benchmark/README.md) to prepare source fingerprints,
compile separate variants, select valid CPU IDs, and invoke repeated trials.
Defaults RX 2 / L2 3 / observer 15 reflect the measured Ryzen topology, not a
universal placement policy. Choose cores present in your process's allowed mask.
The sender and receiver need matching source trees and generated workload bytes.
CPU affinity does not reserve cores from other workloads.

## Hardware receive timestamps

Build target `wire_rx_probe` is Linux-only. Check your NIC's hardware RX support
with `ethtool -T INTERFACE`; ordinary UDP needs a suitable all-packet RX filter.
Use a fresh state path and run this in a separate terminal on the receiver,
replacing `bond0` and the reader with your own names:

```sh
mkdir -p analysis
sudo python3 benchmark/hwtstamp_session.py --interface bond0 \
  --reader "$(id -un)" --state "$PWD/analysis/hwtstamp-session.json"
```

Keep it running after READY. The privileged helper enables RX all, temporarily
grants the reader's private group PHC read access, and restores original
settings/permissions on its stop marker, Ctrl-C or timeout (30 minutes by default).
The corrected helper acknowledges a bond's changeable PHC. Check the active
member's PHC with `ethtool -T`; do not assume the sample `/dev/ptp0` is yours.
No bond-member removal is needed on the measured X550 path.

Prepare the P2 play file on the sender using the linked profile recipes.
On an unbonded interface, omit `--bond`. In another receiver terminal, start the probe:

```sh
build/wire_rx_probe --interface 192.0.2.10 --bond bond0 --phc /dev/ptp0 \
  --packets 125024 --rx-cpu 2 --calibration-cpu 15 \
  --output-prefix analysis/hardware-p2
```

After READY, send the P2 file from the other machine, using the printed port:

```sh
build/play_feed simulator/play_files/pitch-v2-1m-base4m-peak20m-b5ms-e100ms_sym3_seed43.pcap \
  --interface 192.0.2.20 --port PORT_FROM_READY --spin
```

Analyze on the receiver after capture:

```sh
python3 benchmark/analyze_wire_capture.py analysis/hardware-p2
```

The probe requires every expected packet to have hardware and software stamps
for a valid hardware run. It rejects sequence errors, overflow and observed bond
failover. It uses a separate calibration thread and read-only PHC clock reads;
it never adjusts clocks. Analysis correlates PHC and MONOTONIC with bracketing
samples, records uncertainty/residuals and does not extrapolate. Software mode
uses `--software-only` in place of `--phc`.

Stop the privileged helper with Ctrl-C or its printed marker, then confirm
`status=restored` and check interface settings and PHC permissions. The packet
probe does not update L2. Exact physical-wire calibration and wire-to-L2 remain
outside the published measurement. See [the report](experiments/2026-11-12-hardware-rx.md).

## Generated output

Builds, downloaded dependencies, play files, manifests and raw `analysis/` output
are ignored by Git. Runtime reports can contain machine-specific metadata;
review and anonymize any new results before publication. Only the reviewed
experiment tables under `docs/experiments/` are part of this source tree.
