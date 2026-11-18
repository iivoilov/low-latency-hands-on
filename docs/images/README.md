# Regenerating the figures

From the repository root, using Python 3.10+ and Matplotlib (the only additional
charting dependency):

```sh
python3 -m venv build/figures-venv
build/figures-venv/bin/pip install matplotlib==3.10.8
MPLCONFIGDIR="$PWD/build/matplotlib" build/figures-venv/bin/python benchmark/plot_readme_results.py
```

With an existing Matplotlib installation, run
`python3 benchmark/plot_readme_results.py` directly. The figure style uses
DejaVu Sans, selectable SVG text, a white background and explicit units.
Matplotlib 3.10.8 was used for the reviewed figures. A different version may
change layout or SVG bytes without changing values. No new benchmark is run.
Use `--preview-directory build/figure-previews` for untracked PNG previews.

| Figure | Reviewed data source | Selection and statistic |
| --- | --- | --- |
| [Application latency](application-latency.svg) | [Native ring/quantity JSON](../experiments/2026-11-16-ring-quantity-ryzen-results.json) | S1, `variant=both`, GCC/Clang/Rust; median of three per-run nearest-rank p50/p99/p99.9 values, ns converted to µs; whiskers show run min–max |
| [Receive boundaries](receive-boundaries.svg) | [Hardware RX JSON](../experiments/2026-11-12-hardware-rx-results.json) and the native JSON above | Architecture, not a time-scaled plot; RX ranges across three per-packet runs; application ranges across three build medians; different hosts and workloads |
| [Decoder ordering](decoder-ordering.svg) | [Decoder breakdown JSON](../experiments/2026-11-14-decoder-breakdown-results.json) | All three `order-fixtures-1024-*` cases; `100 × (median packed / median original − 1)` across nine paired batches; positive means slower |

The script recomputes application summaries from all 72 per-run rows, checks
sample counts, loss and state/affinity audits, and recomputes the three decoder
bars from their individual batch averages. It prints SHA-256 hashes of the
reviewed input files. The 72 rows include 18 earlier reused controls, not 72 new
trials. No samples are pooled and no receive/application percentiles are added.

The combined native variant is the latest measured variant used in current
source; its results do not prove that both changes improve performance. Older
18-run S&P 500 results are controls. The decoder figure is historical WSL2
decode-only work, not the current receiver decoder. The RX-only probe used a
different host, traffic profile and sample unit; its PHC read bracket is a
diagnostic, not certified physical-wire accuracy.

Only reviewed JSON is needed to regenerate the figures. Rerunning the original
measurements also needs the recorded hardware/toolchains and inputs. Exact
503-stock workloads require an undistributed dated vendor snapshot. Raw timing
samples, captures and source/build snapshots are not distributed with this tree.
