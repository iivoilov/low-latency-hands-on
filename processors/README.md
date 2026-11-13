# C++ OS-socket baseline

`processor_cpp` is the first processor. It receives PITCH multicast through the
normal OS network stack and maintains an internal L2 order book. The matching
[Rust OS-socket implementation](rust/README.md) is also available, with the same
CLI/report schema and shared multicast tests. DPDK is planned; current
measurements use the normal Linux network stack. See
[remote benchmarks](../benchmark/README.md).

## Run

Build through the root CMake project. Start the receiver, then the player in
another terminal:

```sh
build/processor_cpp --interface 127.0.0.1 \
  --warmup-messages 180 --messages 1000000 --output /tmp/cpp-baseline.json
```

```sh
build/play_feed simulator/play_files/pitch-v2-1m-base8m-peak20m-b10ms-e100ms_sym3_seed42.pcap \
  --interface 127.0.0.1 --spin
```

For a separate receiving machine, use its local NIC IPv4 address with `--interface`.
Default group/port are 239.255.0.1:30001. `--port 0` selects a local ephemeral
port and prints it in the `READY` notification. The receiver waits indefinitely
for the first packet; a finite benchmark stops with an error after the configured
idle interval once traffic has started. It exits successfully after collecting
its sample target, finishing the current datagram. Reports are never overwritten.

`--warmup-messages` counts initial **market messages**, not packets or controls.
Use the play-file manifest's `initialization_messages` and `updates` for warmup
and sample target respectively. Defaults are 180 warmup messages and 1,000,000 samples, matching the prepared
three-symbol files. The sample array uses about 8 MB; the L2 book stays bounded.
The C++ executable does not parse the manifest; tests compare its JSON book with
that independent reference. Latency storage is reserved before reception.

## Two-thread design

```text
OS socket -> RX thread -> bounded whole-packet SPSC ring -> L2 thread
                |                  |                         |
            RX timestamp      length, bytes,              decode, orders,
                              RX time, loss count         price aggregation
```

The main thread consumes the ring and owns the book exclusively. One RX thread
blocks in `recvmsg` and normally receives directly into a free ring slot. Entries
hold an entire datagram plus its length and timestamp, so variable-sized PITCH
messages are never split across slots. Default capacity is 4096 packets;
`--ring-slots` accepts a power of two. Release/acquire atomic indices transfer
ownership without a mutex. The consumer releases a slot only after processing
all of its messages. A full ring never overwrites unread data: the RX thread
receives into scratch space and drops the new whole datagram if space is still
unavailable. Ring high-watermark and drop counters are reported.

The consumer busy-waits when the ring is empty. Thread affinity is left to the
OS; this baseline does not select cores or tune the host. Record affinity and
hardware when making performance comparisons. The baseline uses a reserved
`std::unordered_map` for order IDs and ordered `std::map` price levels. Node
allocation/deallocation for book changes remains part of the measured work.

For experiments, the [external affinity launcher](../benchmark/README.md#cpu-placement)
can constrain the existing main L2 thread and RX worker before the sender starts.
It records masks and sampled scheduler counters without changing the C++ binary
or its timing calls. See the [affinity/NUMA experiment](../docs/experiments/2026-11-12-cpu-affinity.md).

## Reader-detected gap queue

The reader/L2 thread compares the received first-message sequence with the next
expected sequence. A bounded forward gap produces one `GapRange` in a separate
SPSC queue, regardless of where input was lost. A heartbeat can bound a gap too.
RX ring/kernel counters remain independent diagnostics; the reader does not
infer or attach a network, NIC, socket, or application cause to a gap.

PITCH numbers messages, so an entry contains `unit`, inclusive `first_sequence`
and `last_sequence`, `message_count`, and `detected_rx_ns` (the RX time of the
packet revealing the gap). Ranges may cross UINT32_MAX to 1; zero is excluded.
The queue records messages absent at detection time. Stale/late packets do not
automatically cancel notifications or repair L2 in this baseline.

`--gap-slots` sets capacity (default 4096, power of two). The reader publishes
with `try_push`; a separate single recovery consumer can call `Processor::pop_gap`.
That method is safe alongside the reader; other Processor state and statistics
remain reader-owned. The two-thread executable has no recovery consumer, so
it drains pending ranges to JSON only at shutdown. Reports include
`pending_gap_ranges`, `gap_ranges_enqueued`, and `gap_ranges_dropped`.

A full gap queue drops the new notification and increments its counter without
overwriting an unread range or blocking L2. Bounded ranges are queued before
benchmark stop-on-gap handling too. Unknown startup prefixes, or drops not
followed by a valid sequence that bounds the hole, do not produce guessed ranges.
Unit Clear preserves queued notifications and loss history. The separate
recovery component is responsible for handling those notifications.

## Timing contract for C++, Rust, and DPDK

Use the system monotonic clock in nanoseconds, currently
[`clock_gettime(CLOCK_MONOTONIC)`](https://man7.org/linux/man-pages/man2/clock_gettime.2.html).
The boundaries are:

1. RX thread: timestamp immediately after successful `recvmsg` returns.
2. Carry that timestamp unchanged in the packet-ring entry.
3. L2 thread: timestamp after each sampled market message has updated both
   order state and price-level aggregates.
4. Store endpoint minus RX timestamp in a preallocated sample array.

`rx_to_l2` therefore includes RX bookkeeping/publication, ring waiting, validation,
decoding, and L2 work. Messages in one packet share an RX timestamp: later
messages include work and instrumentation for earlier messages in that packet.
`queue_wait` separately reports packet dequeue minus RX time as mean/max;
it includes publication and wake/scheduling delay. It is not pure queue occupancy.

This measures elapsed userspace RX-to-update time. Scheduling/preemption is
included. NIC/driver/kernel processing before `recvmsg` returns and time waiting
in the OS socket queue are outside the interval. These numbers are not hardware
NIC-to-L2 latency, thread CPU time, or cycle counts. Clock reading/instrumentation
cost is included and is not subtracted. Nominal clock resolution is recorded;
it is not a claim of measurement accuracy at that resolution.

Time, Unit Clear, and heartbeat messages have no latency samples. Warmup updates
still build the book. Percentiles use nearest rank: `ceil(p * N)`, with no
interpolation. p50/p99/p99.9, min/max/mean, sample counts, losses, ring statistics,
compiler/build, and receive buffer sizes are in JSON. Printing and JSON/file I/O
happen after the threads stop, outside timed processing. The final table and JSON
show best bids descending and asks ascending with quantity and order count.

## Loss policies

`--on-loss stop` is the benchmark default. Ring overflow, reported kernel drops,
or a sequence gap stop the run. Missing input and incomplete sample targets
invalidate the benchmark; latency values from invalid runs must not be compared
as successful workload results.

For loss-tolerant continuous operation:

```sh
build/processor_cpp --interface 127.0.0.1 --continuous --on-loss continue \
  --warmup-messages 180 --messages 1000000 --output /tmp/cpp-continuous.json
```

In this mode the processor continues after ring/kernel loss, forward sequence
gaps, and oversized datagrams. It drops complete datagrams, retains loss
counters, and marks the book incomplete. Lost-order references and reductions
that cannot be applied consistently are counted and skipped while incomplete;
valid later updates still apply. Stale/duplicate packets are ignored rather than
applied twice. A cumulative loss marker travels with each later queued packet,
so a gap outside the queue cannot be hidden by an otherwise valid ring read.

A Unit Clear received through the normal feed resets L2 and restores state
integrity for subsequent updates. The processor never requests a resend or reset.
Continue mode can join mid-session with a partial book; `joined_midstream` marks
that case without guessing how many earlier messages were missed.
Loss history remains recorded, and a run with loss cannot become a valid
benchmark merely because the book recovered. The `book_current` flag means the
applied prefix is contiguous since its last reset; it does not assert that no
newer packets are waiting in the network or ring. TCP resend, GRP gap filling,
and spin/snapshot recovery are outside this project's scope, including the
planned Rust and DPDK versions. The experiment measures ingestion, L2 updates,
and loss behavior without repairing missing input. Unsupported message types,
malformed framing, and fatal OS errors
remain fatal; this implements the simulator's equities subset.

Continuous mode survives quiet markets and keeps processing after its latency
sample window fills; storage stays bounded at `--messages` samples. SIGINT or
SIGTERM stops both threads, prints a snapshot and counters, and writes the report.
It exits successfully on a normal continuous-mode interruption. Reports and loss
counters are emitted at shutdown; live telemetry/display is future work.

The Linux receiver uses [`SO_RXQ_OVFL`](https://man7.org/linux/man-pages/man7/socket.7.html)
when available. These kernel packet counts and PITCH missing-message counts are
separate observations and must not be added together as one loss total. Kernel
counts arrive as ancillary data on later packets. Truncation is detected using
[`recvmsg` flags](https://man7.org/linux/man-pages/man2/recvmsg.2.html).

## Verification

`processor_core` tests incremental L2 transitions, exact percentile/warmup
semantics, full 100k-update reconstruction, strict/continuing gap behavior,
reset recovery, and concurrent variable-length ring wraparound/overflow safety.
`processor_cli` checks argument validation without sockets.
`processor_multicast` tests the full prepared stream against its JSON oracle,
strict/continuing loss, packet truncation, malformed input, timeout, and
continuous-mode shutdown over loopback. Tests use fabricated data only.
