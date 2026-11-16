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
build/prepare_feed
build/prepare_feed --background-mbps 4 --burst-ms 5
build/prepare_feed --background-mbps 8 --burst-ms 25 --burst-every-ms 250
build/prepare_feed --background-mbps 12 --burst-ms 20
```

Other parameters retain defaults: seed 42, unit 1, AAPL/MSFT/SPY, ten levels per
side, three initial orders per level, and eight market events per packet.
Re-running an existing profile fails safely rather than overwriting it.

Example filename:

```text
pitch-v2-1m-base8m-peak20m-b10ms-e100ms_sym3_seed42.pcap
```

`1m` counts updates after initialization (`k` = thousand, `m` = million;
counts that are not exact multiples remain unabridged). `base8m` is normal
background Mbps and `peak20m` is the total Mbps during bursts. `b10ms` is burst
length, `e100ms` is the start-to-start period, `sym3` is symbol count, and
`seed42` is the deterministic seed. Both timing fields are retained so profiles
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
Existing 100k files remain available for quick checks; use `--messages 100000`
to generate one explicitly. The default generator and receiver now use one million.

Version 2 uses the corrected fixed-background interpretation and a
`background_mbps` / `cycle_average_mbps` manifest. Earlier version 1 profiles
are preserved under `legacy-v1/`; they lowered background to meet a fixed
whole-cycle average. Use version 2 files for new experiments. The player follows
either version's stored schedule without changing its meaning. Version 3 adds
explicit per-symbol reference prices, with the same fixed-background schedule.

Archived version 1 names are compact too, for example
`pitch-v1-100k-avg12m-peak20m-b20ms-e100ms_sym3_seed42.json`. Their `avg` field
retains the original whole-cycle average semantics. Renaming does not change
PCAP contents or schedules; each JSON's `pcap` field points to its renamed file.

## Broad stock universe

Use a separate subdirectory for each dated universe/price snapshot and seed.
Download the public IVV holdings CSV and a current S&P 500 constituent CSV into
an ignored analysis directory, then freeze their intersection and provenance:

```sh
python3 simulator/import_sp500.py \
  --holdings analysis/sp500-2026-11-16/ivv-holdings.csv \
  --constituents analysis/sp500-2026-11-16/constituents.csv \
  --directory simulator/play_files/sp500-20261113-seed44
build/prepare_feed --directory simulator/play_files/sp500-20261113-seed44 \
  --universe simulator/play_files/sp500-20261113-seed44/universe.csv \
  --seed 44 --background-mbps 8 --burst-mbps 20 --burst-ms 10 --burst-every-ms 100
build/prepare_feed --directory simulator/play_files/sp500-20261113-seed44 \
  --universe simulator/play_files/sp500-20261113-seed44/universe.csv \
  --seed 44 --background-mbps 8 --burst-mbps 20 --burst-ms 25 --burst-every-ms 250
```

Sources: [iShares IVV holdings](https://www.ishares.com/us/products/239726/ishares-core-s-p-500-etf/latest-holdings.csv)
and the [S&P 500 constituent CSV](https://github.com/datasets/s-and-p-500-companies/blob/main/data/constituents.csv).
The importer requires prices for every constituent and at least 500 unique
symbols. It excludes nonmembers and warrants, normalizes BRK B/BF B to
BRK.B/BF.B, and records the source date and SHA-256 hashes in `provenance.json`.
Vendor holdings prices are reference anchors, not verified official closing
prints. Retain original downloads outside source control; subsequent downloads
can change, so they cannot reproduce a past snapshot without its archived inputs.

This set contains 503 symbols, 30,180 initialization adds, 1,000,000 measured
updates and 128,774 packets per profile. Use `--warmup-messages 30180` for a
benchmark receiver; the remote runner reads this from the manifest. Version 3
names begin `pitch-v3-1m-base8m-peak20m-` and contain `sym503_seed44`.

Each order slot is equally likely to be selected; depleted orders are refilled
on the next event. With equal book geometry this spreads activity broadly
across stocks, including independent bid/ask/last decisions. The action mix,
sizes and bounded price bands remain synthetic. This does not fit empirical
per-stock activity, correlations, spreads or volatility. At a one-second human
monitor throttle, many independent changes can still conflate onto the same
timer boundary; a larger universe alone does not remove that display effect.
