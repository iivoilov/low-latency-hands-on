# Prepared play files

Run `prepare_feed` from the repository root to create workloads here. Each
profile is a deterministic PITCH PCAP plus a same-stem JSON manifest. Generated
files are ignored by git. No purchased Cboe data is included.

Profiles for the first experiments (each has 1,000,000 updates plus initialization):

| Background Mbps | Peak Mbps | Burst duration | Burst period | Frequency | Cycle average Mbps |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 20 | 10 ms | 100 ms | 10 Hz | 9.2 |
| 4 | 20 | 5 ms | 100 ms | 10 Hz | 4.8 |
| 8 | 20 | 25 ms | 250 ms | 4 Hz | 9.2 |
| 12 | 20 | 20 ms | 100 ms | 10 Hz | 13.6 |

```sh
build/prepare_feed --seed 43
build/prepare_feed --seed 43 --background-mbps 4 --burst-ms 5
build/prepare_feed --seed 43 --background-mbps 8 --burst-ms 25 --burst-every-ms 250
build/prepare_feed --seed 43 --background-mbps 12 --burst-ms 20
```

The recipes select seed 43; other parameters retain defaults: unit 1, AAPL/MSFT/SPY, ten levels per
side, three initial orders per level, and eight market events per packet.
Re-running an existing profile fails safely rather than overwriting it.

Example filename:

```text
pitch-v2-1m-base8m-peak20m-b10ms-e100ms_sym3_seed43.pcap
```

`1m` counts updates after initialization (`k` = thousand, `m` = million;
counts that are not exact multiples remain unabridged). `base8m` is normal
background Mbps and `peak20m` is the total Mbps during bursts. `b10ms` is burst
length, `e100ms` is the start-to-start period, `sym3` is symbol count, and
`seed43` is the deterministic seed. Both timing fields are retained so profiles
with different burst lengths or frequencies have distinct names.

Full symbol identities, PITCH unit, levels, orders per level, and events per
packet are recorded in the JSON instead of the filename. If profiles share the
visible name parameters, choose a different `--directory`; the preparer refuses
to overwrite existing files. The old `c7620fa3d8a72db7` was a hash of the ordered
AAPL/MSFT/SPY list; `u1` meant PITCH unit 1. Neither appears in the compact names.

Background remains fixed; longer/more frequent bursts raise the whole-run
average. Rates count PITCH UDP payload bits only. NIC-counter readings may
include additional protocol overhead.

The PCAP stores nanosecond relative send deadlines. A zero-message heartbeat
ends the schedule after the final packet's byte budget. Each manifest contains
the configured background/peak, derived cycle average, finite-file average/duration,
message/packet/byte counts,
`initialization_packets`, and `final_book` L2 reference levels. JSON prices use
units of 0.0001. Final bids are sorted descending and asks ascending.

The processor should use initialization to build its book, measure the following
1,000,000 market updates, and compare its final aggregation with `final_book`.
Protocol control messages and the final heartbeat are not latency samples.
At 1,000,000 update samples, 1,000 observations lie above the nearest-rank
p99.9 threshold. Repeated runs remain useful because neighboring messages
share packets and burst conditions. The default schedule lasts about 22.3 seconds.
Generate quick checks with `--messages 100000`; benchmark profiles and the
receiver default use one million updates. Workloads are generated locally and
are not shipped in this repository.

Version 2 uses fixed background load. Replay also accepts older version 1
schedules, which compensated background to hold an overall average, but all
published new profile recipes use version 2.
