# Rust OS-socket processor

Linux Rust implementation of the same two-thread PITCH/L2 processor and timing
contract as the C++ baseline. It uses `recvmsg`, `SO_RXQ_OVFL`, a bounded
whole-packet SPSC ring, reader-side gap detection, and a separate bounded SPSC
gap queue. Loss, continuity, initialization, warmup, sample counts, and JSON fields
match the [shared processor contract](../README.md).

Build from the project root:

```sh
python3 benchmark/build_rust.py
```

The executable is `build/rust-o3/processor_rust`; the build helper also stores
toolchain versions, source hashes, the command and full compiler log. It uses the
installed Rust toolchain, Cargo.lock, and Release opt-level 3, without LTO or
`target-cpu=native`. `--cargo` and `--rustc` accept explicit tool paths. Optional
`--vendor-directory` supports offline builds from checksummed dependency sources;
vendored artifacts belong under `build/`.

Direct loopback usage (start before the player):

```sh
build/rust-o3/processor_rust --interface 127.0.0.1 \
  --warmup-messages 180 --messages 1000000 --output /tmp/rust-baseline.json
```

For measurements use the [remote runner](../../benchmark/README.md), whose default
baseline now pins RX to CPU 2 and L2 to CPU 3. Direct processor invocation inherits
OS placement. The external launcher works with either language because the main
thread owns L2 and there is exactly one RX worker.

Verify native behavior and the same CLI/multicast cases used for C++:

```sh
CARGO_TARGET_DIR="$PWD/build/rust-tests" cargo test --manifest-path processors/rust/Cargo.toml --locked
python3 processors/cpp/tests/test_cli.py build/rust-o3/processor_rust build/prepare_feed build/play_feed
python3 processors/cpp/tests/test_cli.py build/rust-o3/processor_rust build/prepare_feed build/play_feed --network
```

The packet/gap queues share one SPSC implementation with cache-separated atomic
indices and release/acquire ownership transfer. Unique endpoints and lifetime
guards prevent publishing twice or reusing a slot while a reader holds it. The
small unsafe boundary covers `UnsafeCell` slot access and Linux FFI; safety
invariants are documented in code. `Processor::take_gap_consumer` transfers the
single recovery-consumer endpoint; no recovery worker is included. Concurrent
tests cover variable-length packet wraparound and gap FIFO preservation.

Orders use standard `HashMap<u64, Order>` with `RandomState`, reserving capacity
for the configured live-order limit (one million by default). Symbols and price
levels use `BTreeMap`. These have different
hashing, layout and allocation behavior from C++ `unordered_map` / `map`; the
comparison measures these implementations and toolchains, not language alone.
Both books accept `--max-live-orders` and prune empty symbols. The limit counts
simultaneously live orders, not IDs seen during the day. Destination
filtering and fatal-versus-skippable input errors follow the shared contract;
sender addresses are not checked. C++ uses secret-keyed SipHash-2-4 with no
bucket-length rejection; Rust retains `RandomState`. The initial
2026-11-13 hardening experiment used a C++ `map` for order IDs; those results are
historical. The [current native comparison](../../docs/experiments/2026-11-13-siphash.md)
records the selected C++ baseline and unchanged Rust reference.
Book allocations remain timed. Sample storage is reserved before reception and
bounded at one million entries by default. JSON serialization, sorting and file
output run after the processing threads stop. `libc` supplies platform ABI;
`serde_json` and its dependencies are confined to reporting, outside timed updates.

## Input validation and resource protection

The [shared security behavior](../README.md#input-validation-and-resource-protection)
is part of the processor implementation: bounded framing/field validation,
destination filtering without sender-IP trust, bounded queues/samples/live
orders, and pruning empty book state. These checks remain active after loss;
only the documented missing-reference and positive-overdraw cases are skippable.
Rust's standard `HashMap` uses randomized keyed hashing selected for HashDoS
resistance; it accepts colliding keys normally and has no 16-entry cutoff.
This gives practical chosen-ID resistance, not a strict worst-case lookup bound
or feed authentication. The live-order budget remains independent of hashing.
