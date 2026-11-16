# Zero-copy packed PITCH decoder

Experiment date: 2026-11-14. The new `decode_pitch_message_zero_copy` reads
packed wire layouts by reference. It avoids the intermediate wire-object copies
in `decode_pitch_message_packed`. In this local build it lowers median decoding
time against the byte decoder in all 40 conditions and against the copying
packed decoder in 35. The natural-order PCAP improves from **11.359 to 8.759
ns/message** versus byte decoding, and from **9.180 to 8.759** versus packed
copying. The 23-layout cyclic fixture improves from **6.529 / 7.176 to 5.020**.
These are decoder-only batch measurements.

## Implementation

The [new decoder](../../processors/cpp/src/pitch_message_zero_copy.cpp) first
reads the two-byte message prefix, then selects a concrete packed layout through
its type switch. Each case uses a `const auto&` view directly into the message
buffer, for example:

```cpp
const auto& wire = view<Executed>(bytes);
m.time_offset = from_le(wire.header.time_offset);
m.order_id = from_le(wire.order_id);
m.quantity = from_le(wire.quantity);
```

The existing named layouts were moved unchanged into a
[shared private header](../../processors/cpp/src/pitch_wire_layouts.hpp).
`#pragma pack(1)` gives each selected layout alignment one; compile-time checks
cover protocol sizes and important offsets. Fields are read by value from packed
members, without binding naturally aligned integer references. `from_le` remains
an endian conversion and compiles to an identity on this little-endian machine.

A plain `reinterpret_cast` alone would not start the C++ object lifetime. The
view uses non-allocating placement `new (storage) unsigned char[sizeof(Wire)]`
**without an initializer**, then launders the typed pointer. C++20 allows this
byte-array operation to implicitly create suitable implicit-lifetime objects
in the storage; it does not initialize or copy the bytes. This follows
[WG21 P0593R6 and its CWG2382 discussion](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p0593r6.html).
The API therefore accepts writable `span<uint8_t>` storage. No reference escapes
the decoder, and each selected view checks its exact extent. A maximum-sized
union is not formed over a shorter message; there is no inactive union member
to read.

GCC 12.2 `-O3` removes the lifetime marker and inlines the view. Disassembly shows
direct packet loads, with no intermediate wire-object stack copy. For example,
Time seconds is loaded with `mov eax, DWORD PTR [rsi+0x2]` and stored directly
to the normalized result. The former Time and Reduce Long copy/store/reload
paths are gone. No `-fno-strict-aliasing` or `may_alias` workaround is used.
The object file still contains allocation/copy symbols for exception/string
handling; they are not wire-object copies on valid decoding paths.

All three versions still validate fields and return the same normalized
`PitchMessage` by value. This is zero-copy **wire access**, not zero-copy socket
reception or removal of result normalization. At measurement time the processor
still selected the original byte decoder. Following this experiment, the
zero-copy decoder was adopted for the C++ receive path; reports now record
`pitch_decoder: "packed-zero-copy"`. The ring consumer exposes exclusively
owned writable storage to `Processor::process` until slot release. All ten
CTest tests, including multicast, passed after adoption; processor/L1/decoder
checks also passed under UBSan. No new end-to-end latency measurement was made.
The archived source hashes describe the benchmark build before adoption.
The packed representation is a compiler extension; performance
and code generation below were checked on GCC/x86-64 only.

## Method and verification

Local Core i9-13900K, WSL2 Linux 6.6.87.2, GCC 12.2.0, C++20 Release `-O3
-DNDEBUG`, usual project warnings, no LTO or `-march=native`. The benchmark is
pinned to guest CPU 3 and the observer to CPU 15. Each batch verifies its CPU
at both ends and records the allowed mask. No CPU isolation, host tuning or
remote deployment was performed. Guest affinity does not reserve a physical
Windows-host core.

The same executable compares `original`, `packed` and `zero-copy`. The mutable
versus read-only function signature is selected once per batch, with a direct
function-pointer call per message in either loop. All versions do the same
normalized-result checksum work. Preflight compares all decoded fields before
timing, and each round checks equal output checksums. File I/O, corpus creation
and output are outside the interval.

The 40 conditions include every one of the 23 fixture layouts, seven types from
the synthetic PCAP, and ten mixed/order controls. Each condition has nine rounds
with all three decoder orders shuffled deterministically using seed 20261114.
Each decoder receives at least 100,000 warmup calls and 5,000,000 calls per
measured batch, rounded up to full corpus passes. Tables report medians of
`CLOCK_MONOTONIC` batch-average ns/message, not message latency percentiles.
Every condition completed on its first attempt: **1,080 timed batches**, no
excluded attempts or retries. Tests/builds did not run during timing.

Correctness checks compare all fields and exact rejection messages across the
three decoders, and verify that input bytes remain unchanged: **169,350 cases**.
These cover all byte values in every fixture field, random mutations, extensions,
truncations, starts at offsets 0–15, and short/truncated layouts ending immediately
before an inaccessible guard page. All eight offline CTest tests and both
loopback multicast tests passed. The three decoder/benchmark tests also passed
under UBSan with recovery disabled; all **29 Python benchmark-tool tests** passed.
ASan was not used for this run.

Raw stdout/stderr, commands, source snapshots, disassembly, compiler flags and
hashes remain in `analysis/2026-11-14-zero-copy-decoder-001/`. Portable
[JSON](2026-11-14-zero-copy-decoder-results.json) includes all 1,080 batch records,
checksums, counts, order hashes, alignments and source/input/binary hashes;
[CSV](2026-11-14-zero-copy-decoder-results.csv) contains the condition summaries.
These fresh three-way measurements should be compared within this build, rather
than combining absolute values from earlier two-way sessions.

## Individual fixture layouts

Each condition repeatedly reads a single fixed fabricated message. Negative
percentages mean lower elapsed time for zero-copy. Round counts preserve the
consistency of each comparison rather than implying every median is a stable
win.

| Layout (type, bytes) | Byte ns | Packed copy ns | Zero-copy ns | Zero vs byte | Zero vs copy | Zero faster rounds: byte / copy |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| time (`0x20`, 6 B) | 2.948 | 6.128 | 2.468 | -16.3% | -59.7% | 9/9 / 9/9 |
| time epoch (`0x20`, 10 B) | 3.414 | 6.281 | 2.598 | -23.9% | -58.6% | 9/9 / 9/9 |
| add long (`0x21`, 34 B) | 9.914 | 9.696 | 6.695 | -32.5% | -30.9% | 9/9 / 9/9 |
| add short (`0x22`, 26 B) | 8.976 | 9.455 | 6.862 | -23.5% | -27.4% | 9/9 / 9/9 |
| execute (`0x23`, 26 B) | 5.349 | 6.486 | 2.662 | -50.2% | -59.0% | 9/9 / 9/9 |
| execute price (`0x24`, 38 B) | 6.460 | 7.849 | 2.811 | -56.5% | -64.2% | 9/9 / 9/9 |
| reduce long (`0x25`, 18 B) | 4.405 | 6.985 | 2.604 | -40.9% | -62.7% | 9/9 / 9/9 |
| reduce short (`0x26`, 16 B) | 4.201 | 3.151 | 3.143 | -25.2% | -0.3% | 9/9 / 6/9 |
| modify long (`0x27`, 27 B) | 5.171 | 7.011 | 3.344 | -35.3% | -52.3% | 9/9 / 9/9 |
| modify short (`0x28`, 19 B) | 4.602 | 6.721 | 3.188 | -30.7% | -52.6% | 9/9 / 9/9 |
| delete (`0x29`, 14 B) | 3.969 | 3.151 | 2.981 | -24.9% | -5.4% | 9/9 / 9/9 |
| trade long (`0x2A`, 41 B) | 11.019 | 9.633 | 6.895 | -37.4% | -28.4% | 9/9 / 9/9 |
| trade short (`0x2B`, 33 B) | 9.689 | 9.606 | 6.949 | -28.3% | -27.7% | 9/9 / 9/9 |
| trade break (`0x2C`, 14 B) | 3.966 | 2.608 | 2.616 | -34.0% | +0.3% | 9/9 / 5/9 |
| end session (`0x2D`, 6 B) | 3.434 | 2.780 | 2.775 | -19.2% | -0.2% | 9/9 / 3/9 |
| add expanded (`0x2F`, 41 B) | 11.032 | 10.548 | 7.599 | -31.1% | -28.0% | 9/9 / 9/9 |
| trade expanded (`0x30`, 43 B) | 12.072 | 10.062 | 7.531 | -37.6% | -25.2% | 9/9 / 9/9 |
| status (`0x31`, 18 B) | 11.289 | 10.471 | 10.906 | -3.4% | +4.2% | 8/9 / 1/9 |
| auction update (`0x95`, 47 B) | 10.297 | 7.978 | 7.614 | -26.1% | -4.6% | 9/9 / 8/9 |
| auction summary (`0x96`, 27 B) | 9.125 | 7.675 | 7.332 | -19.7% | -4.5% | 9/9 / 9/9 |
| unit clear (`0x97`, 6 B) | 3.207 | 2.404 | 2.438 | -24.0% | +1.4% | 9/9 / 0/9 |
| retail (`0x98`, 15 B) | 7.807 | 7.373 | 7.423 | -4.9% | +0.7% | 9/9 / 4/9 |
| time reference (`0xB1`, 18 B) | 3.990 | 7.009 | 2.969 | -25.6% | -57.6% | 9/9 / 9/9 |

The large copying regressions are eliminated: Time is 59.7% lower than packed
copying, Time Reference 57.6%, Reduce Long 62.7%, and both Modify layouts about
52%. Execute Price falls 64.2%. This supports the earlier stack-copy diagnosis.

Zero-copy is not faster than packed copying for every layout. Trading Status
is **4.2% slower** and Unit Clear **1.4% slower** in these medians. Trade Break
and Retail differ by less than 1% with mixed round directions. Some paths already
had efficient code in the copying implementation; removing source-level copies
does not guarantee a faster machine-code layout or branch behavior.

## Filtered PCAP types

The generated seed-43 PCAP contains 1,000,204 messages and 24,667,328 compacted
wire bytes: 1,000,180 order messages, 23 Time messages and one Unit Clear. Type
selection preserves backing bytes and positions while changing the active views.
The backing allocation therefore remains the same for each filtered condition.
These vary values and addresses rather than repeatedly reading one fixed fixture.

| Filtered type | Byte ns | Packed copy ns | Zero-copy ns | Zero vs byte | Zero vs copy | Zero faster rounds: byte / copy |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| pcap-type-20 | 2.653 | 5.935 | 2.238 | -15.6% | -62.3% | 9/9 / 9/9 |
| pcap-type-21 | 13.325 | 12.275 | 9.735 | -26.9% | -20.7% | 9/9 / 8/9 |
| pcap-type-23 | 7.901 | 8.216 | 4.935 | -37.5% | -39.9% | 9/9 / 9/9 |
| pcap-type-25 | 7.361 | 8.926 | 5.757 | -21.8% | -35.5% | 8/9 / 8/9 |
| pcap-type-27 | 7.422 | 8.145 | 5.503 | -25.9% | -32.4% | 8/9 / 9/9 |
| pcap-type-29 | 9.614 | 5.719 | 5.443 | -43.4% | -4.8% | 8/9 / 7/9 |
| pcap-type-97 | 3.300 | 2.474 | 2.496 | -24.4% | +0.9% | 9/9 / 1/9 |

## Mixed inputs and ordering

`--copies 1024` repeats descriptors, not the wire bytes. The five order-fixture
layouts match the PCAP's order types with equal weights. Input/grouped/shuffled
variants preserve bytes, relative offsets, counts and alignment histograms within
each set. The backing allocation modulo 64 is 48 for fixtures and 16 for the PCAP
in this run. The fixture bytes fit in cache; the PCAP is about 24.7 MB. Shuffled
PCAP results include changed locality as well as dispatch order.

| Condition | Byte ns | Packed copy ns | Zero-copy ns | Zero vs byte | Zero vs copy | Zero faster rounds: byte / copy |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| fixtures-1024-grouped | 6.604 | 7.200 | 4.912 | -25.6% | -31.8% | 9/9 / 9/9 |
| fixtures-1024-input | 6.674 | 6.849 | 5.013 | -24.9% | -26.8% | 9/9 / 9/9 |
| fixtures-1024-shuffled | 15.683 | 12.203 | 11.846 | -24.5% | -2.9% | 9/9 / 9/9 |
| fixtures-23-input | 6.529 | 7.176 | 5.020 | -23.1% | -30.0% | 9/9 / 9/9 |
| order-fixtures-1024-grouped | 5.563 | 6.809 | 3.770 | -32.2% | -44.6% | 9/9 / 9/9 |
| order-fixtures-1024-input | 5.932 | 7.121 | 3.271 | -44.9% | -54.1% | 9/9 / 9/9 |
| order-fixtures-1024-shuffled | 11.515 | 8.947 | 8.578 | -25.5% | -4.1% | 9/9 / 9/9 |
| pcap-grouped | 10.395 | 10.047 | 7.251 | -30.2% | -27.8% | 9/9 / 9/9 |
| pcap-input | 11.359 | 9.180 | 8.759 | -22.9% | -4.6% | 9/9 / 7/9 |
| pcap-shuffled | 51.638 | 38.398 | 32.420 | -37.2% | -15.6% | 9/9 / 8/9 |

The cyclic-fixture regression seen with copying packed structs is removed.
Zero-copy is faster than the byte decoder in all nine rounds for both the
23-layout cycle and the natural-order PCAP. Its PCAP advantage over packed
copying is smaller: **4.6%** by medians, with seven of nine rounds faster and
individual round changes ranging from **-10.7% to +3.3%**. Shuffled PCAP medians
improve 15.6% versus copying, but one round regresses. The JSON/CSV preserve
those ranges. No hardware performance counters were collected, so detailed
branch/cache explanations remain hypotheses.

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s benchmark -p 'test_*.py'

# INPUT.pcap is the version-2 million-update, seed-43, base-8/peak-20 Mbps file.
python3 benchmark/run_decode_breakdown.py --pcap INPUT.pcap \
  --include-zero-copy --cpu 3 --observer-cpu 15 \
  --messages 5000000 --warmup 100000 --repeats 9 --seed 20261114 \
  --output analysis/zero-copy-NEW-ATTEMPT

# One three-way condition, without the matrix launcher:
taskset -c 3 build/pitch_decode_bench \
  --fixtures processors/tests/equities_messages.tsv \
  --decoders original,packed,zero-copy \
  --messages 5000000 --warmup 100000 --repeats 9
```

The input SHA-256 is
`de3b81eb80546381be8ffa84f00c713a9ba921c12d24abad0e23cabbb437babe`.
As in the earlier studies, no packets are sent during this benchmark and loss
is not applicable. These measurements do not establish an RX-to-L1 improvement
or performance on another compiler, native Linux host, or processor.
