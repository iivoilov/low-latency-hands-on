# AI-assisted engineering: evidence and responsibility

The maintainer confirmed on 19 November 2026 that Codex researched techniques,
implemented changes, tested them and ran benchmarks in the two episodes below.
The engineer directed scope and reviewed conclusions. This is an attributed
account of development, not an autonomy metric inferred from commit authors.
The repository's reports and scripts establish what was implemented and measured;
they do not independently identify who issued every command or reviewed every step.

## Decoder investigation: test an explanation, preserve counterexamples

The [14 November breakdown](experiments/2026-11-14-decoder-breakdown.md) went
beyond an aggregate decoder result. Codex extended the benchmark to isolate
message layouts and reorder the same contents, checked field equivalence and
checksums, inspected disassembly, and tested a separate scalar-member diagnostic.
The [runner](../benchmark/run_decode_breakdown.py) and
[diagnostic patch](experiments/2026-11-14-decoder-scalar-diagnostic.patch)
make those technical steps inspectable.

Nine paired repetitions of five order layouts showed the packed-copy decoder
21.5% slower in cyclic order and 19.8% faster when shuffled. Some isolated
layouts improved; others regressed. The separate diagnostic passed 168,784
decoder-equivalence cases and supported compiler-generated temporary copies as
one explanation. Hardware counters were unavailable, so an exact stall or
branch-misprediction attribution was not established.

The engineering decision was to retain those distinctions: the scalar diagnostic
was not adopted as the production decoder by that study, and its local gains
were not reported as a full-receiver speedup. Today's C++ zero-copy decoder has
its [own separate study](experiments/2026-11-14-zero-copy-decoder.md).
The published evidence records 40 main and 14 diagnostic conditions, including
negative outcomes. This demonstrates evaluation of a proposed optimization;
it is not evidence of an independent reviewer agent or an unsupervised agent team.

## Ring and quantity changes: isolate, combine, validate

The [16 November native comparison](../benchmark/README.md#native-ring-and-quantity-comparison-2026-11-16)
tested cached queue positions and in-place price-level quantity updates separately
and together in C++ and Rust. Codex implemented and tested the changes, orchestrated
the compiler/profile matrix and analyzed the reports. The engineer selected the
scope and requested reuse of the earlier 18 controls.

The saved [JSON](experiments/2026-11-16-ring-quantity-ryzen-results.json)
retains 54 new valid trials, 18 reused controls, source/binary hashes,
per-run timings, and audits of affinity and final state. Runtime-source equality
was checked before using the controls; they were measured earlier that day and
were not interleaved with the changed variants. Each new trial matched independent
wire reconstructions of final L1/L2 state and decision counts. Local WSL2
measurements remain [separate](experiments/2026-11-16-ring-quantity-local-results.json).

Rust's quantity-only variant showed a small gain, while the combination did not
retain it. The resulting conclusion rejects an overall speedup claim for both
changes, despite successful correctness checks. Only 2.4325% of measured updates
exercise the new positive same-price modification path; that limits generalization.

## What this account does and does not establish

These episodes show AI assistance in research, implementation, automated
verification, benchmark orchestration and critical analysis. The reports verify
technical outcomes; the maintainer confirms role attribution. They do not
establish a productivity multiplier, a rate of autonomous completion, or separate
agent authorship of implementation and evaluation.

AI accelerates exploration and implementation. The engineer remains responsible
for architecture, correctness, experiment authorization and interpretation,
including retaining unsuccessful attempts and accepting performance conclusions
only within the measured boundaries.
