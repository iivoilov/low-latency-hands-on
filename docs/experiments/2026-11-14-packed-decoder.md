# Named packed layouts versus byte extraction

On 2026-11-14, a second C++ equities PITCH decoder was added as
`decode_pitch_message_packed`. On the local GCC 12.2 / WSL2 setup, it reduced
decoding time for the existing million-update PCAP but increased time for a
small cyclic corpus covering every supported layout. These are decoder-only
measurements; they do not establish a change in RX-to-L1 latency.

The [message-type and ordering follow-up](2026-11-14-decoder-breakdown.md)
identifies individual faster/slower paths and investigates compiler-generated
temporary copies with an independently validated scalar-member diagnostic.

## Implementation

The existing `decode_pitch_message` and its default use by the processor are
retained. The alternative is in
[`pitch_message_packed.cpp`](../../processors/cpp/src/pitch_message_packed.cpp).
It uses named wire fields in `#pragma pack(push, 1)` structures, including
short, long and expanded orders/trades, both execution forms, clocks, controls,
status, auctions and retail indicators. Nested order layouts share field names
and specialize quantity/price widths and symbol length. Add versus Trade side
validation is a compile-time template choice, known from the dispatch case.

Every documented prefix length and important field offset has a compile-time
assertion. A length check precedes `memcpy` into a live local wire object. There
are no casts of packet pointers to structs and no inactive-union reads. Integer
members are converted explicitly from little endian. Native-width loads are
possible on this little-endian target; a byte-swap path supports big-endian
compilation but has not been exercised on big-endian hardware.

The alternative dispatches once into message-specific code rather than first
selecting a minimum size and then choosing fields through conditional chains.
Both return the same normalized `PitchMessage` and perform the same input
validation. The comparison therefore includes dispatch/control-flow changes as
well as packed field loads. It does not isolate packing alone as the cause of
any performance difference. Disassembly is preserved with the raw results;
there is no out-of-line `memcpy` call in the packed decoder's hot function.

## Correctness

The shared 23 fixtures cover all 22 equities message types and both Time forms.
`pitch_decoder_tests` checks complete field equality and matching error messages
between the decoders for **168,784 cases**: original fixtures, starts at offsets
0 through 15, malformed/self-consistent truncations, extensions through 255
bytes, every possible value at every field-byte position, and deterministic
multi-byte mutations using seed 20261114. Existing independent fixture checks
and L2/L1 behavioral tests remain active.

Release CMake build and all **9 CTest tests**, including loopback multicast,
passed. The new decoder equivalence and benchmark smoke tests also passed in
a separate Debug UBSan build with `-fsanitize=undefined
-fno-sanitize-recover=all -fno-omit-frame-pointer`. No ASan claim is made; the
earlier local ASan runtime limitation is recorded in the
[L1 verification notes](../pitch-l1.md#verification).

Before timing, the benchmark compares every decoded field for every message
in its input corpus. Timed/warmup runs also compare output checksums. All
recorded invocations completed successfully; there were no automatic retries
or omitted timing trials.

## Method

The machine is a Core i9-13900K running WSL2, Linux 6.6.87.2. Compilation uses
GCC 12.2.0, C++20, `-O3 -DNDEBUG`, the project's warning flags, no LTO and no
`-march=native`. Both functions are out of line, compiled in separate translation
units with the same settings. The single benchmark thread is launched with
`taskset -c 3`; the Python launch/metadata process uses CPU 15. Guest CPU 3 has
reported SMT siblings 2-3. No governor, NIC, host scheduling, package installation
or remote deployment changes were made. Other host activity is not excluded.

There are nine paired repetitions per corpus. A deterministic MT19937 stream
with seed 20261114 selects original-first or packed-first independently for each
pair; every pair uses identical input and call counts. Full-corpus warmup runs
precede the paired measurements. No builds or tests run during the timing batch.

`CLOCK_MONOTONIC` brackets a whole batch. The measured loop includes input-view
construction, the complete decoder call/normalized result, and the same small
checksum accumulation for both versions. File reading, corpus allocation,
cross-decoder correctness checks, warmup and output are outside the interval.
There is no socket, packet ring, pacing, L2 book or L1 decision in this benchmark.
Individual-message clock calls would distort these small intervals, so results
are batch-average ns/message. Table medians and ranges describe nine such batch
averages; they are not per-message p50/p99/p99.9 percentiles. Clock cost is not
subtracted.

| Corpus | Messages in corpus | Message bytes | Timed calls per decoder/run | Warmup calls per decoder |
| --- | ---: | ---: | ---: | ---: |
| All-layout fixtures, repeated in file order | 23 | 543 | 5,000,016 | 100,004 |
| Existing version-2, seed-43, million-update PCAP | 1,000,204 | 24,667,328 | 5,001,020 | 1,000,204 |

The benchmark rounds requested counts up to complete corpus passes. The PCAP
contains 1,000,180 order messages, 23 Time messages and one Unit Clear. Its final
zero-message heartbeat creates no decoder call. The workload was prepared with
an 8 Mbps background, 20 Mbps peak and 10 ms bursts every 100 ms, but playback
timing is unused here. The corpus stores message bytes compactly with offset/
length descriptors; it does not retain transport headers or packet boundaries.
The small cyclic fixture corpus and the larger order-update corpus have different
message mixes, prediction patterns and cache footprints.

## Results

Final session (003), with affinity `[3]` read inside the executable, unchanged at exit,
and CPU 3 recorded before/after every measured batch:

| Corpus | Original median ns/message | Packed median ns/message | Packed time change |
| --- | ---: | ---: | ---: |
| All layouts, cyclic | 6.598 | 7.069 | +7.1% |
| Million-update PCAP | 11.118 | 9.042 | -18.7% |

| Corpus / decoder | Minimum batch average | Maximum batch average |
| --- | ---: | ---: |
| All layouts / original | 6.381 ns | 9.498 ns |
| All layouts / packed | 6.995 ns | 7.236 ns |
| PCAP / original | 10.982 ns | 11.315 ns |
| PCAP / packed | 8.737 ns | 9.596 ns |

The original fixture maximum of 9.498 ns remains included; no outlier filtering
was used. Two earlier sessions used the same requested affinity, corpus and run
counts. Their medians are retained separately:

| Earlier session / corpus | Original ns/message | Packed ns/message | Packed time change |
| --- | ---: | ---: | ---: |
| 001 / all layouts, cyclic | 6.561 | 6.840 | +4.2% |
| 001 / million-update PCAP | 10.756 | 9.184 | -14.6% |
| 002 / all layouts, cyclic | 6.605 | 7.003 | +6.0% |
| 002 / million-update PCAP | 11.204 | 9.069 | -19.1% |

The initial launcher sampled the child's affinity immediately after spawn,
before `taskset` necessarily executed; its recorded child mask is not reliable
evidence of measured placement. In-process affinity/endpoint recording was
then added outside the timed intervals for session 002. An assembly review
subsequently found that the packed decoder's shared order helper called the
out-of-line `PitchMessage::trade()` predicate even though each dispatch case
already knows whether it handles an Add or Trade. The original decoder inlined
this predicate within its own translation unit. The packed helper was changed
to take that choice as a compile-time template parameter; equivalence/UBSan and
the full CTest suite were rerun before session 003. Final disassembly confirms
that the packed decoder no longer calls `trade()`.

These are separate binary identities and sessions, not pooled samples or
retries chosen to hide an unfavorable result. All three sessions show the same
direction for each corpus, with variation in the magnitude of the difference.
Removing the unnecessary predicate call did not eliminate the all-layout
regression; these measurements do not isolate its individual timing effect.

The packed version is an experimental alternative. These results support a
decoding benefit for this synthetic order-update mix on this build, with a
regression on the cyclic all-layout mix. WSL2, unisolated host activity, the
single compiler/CPU, compacted corpus layout and loop overhead limit broader
conclusions. Native runs and the complete RX-to-L1 path have not been measured
with the alternative.

## Reproduce and artifacts

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j --target pitch_decode_bench pitch_decoder_tests
ctest --test-dir build -R '^pitch_decoder' --output-on-failure

taskset -c 3 build/pitch_decode_bench \
  --fixtures processors/tests/equities_messages.tsv \
  --messages 5000000 --warmup 100000 --repeats 9 --seed 20261114

taskset -c 3 build/pitch_decode_bench \
  --pcap simulator/play_files/pitch-v2-1m-base8m-peak20m-b10ms-e100ms_sym3_seed43.pcap \
  --messages 5000000 --warmup 100000 --repeats 9 --seed 20261114
```

Portable [JSON](2026-11-14-packed-decoder-results.json) and
[CSV](2026-11-14-packed-decoder-results.csv) retain all **108 timed batches** from
all three sessions, message-type counts, source/input/binary SHA-256 identities,
repetition/order, elapsed time and checksums. Raw inventories, actual compile
commands, invocation commands, stderr and JSON remain under
`analysis/2026-11-14-packed-decoder-local-001/`, `-002/` and `-003/`; the last two
directories also contain disassembly. Exact packed-decoder source snapshots
are retained for all sessions, with hashes checked against their original
metadata. Source identities describe the uncommitted working
tree used for this experiment.
