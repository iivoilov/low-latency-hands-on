# Project structure

| Path | Purpose |
| --- | --- |
| `simulator/src/prepare.cpp` | Deterministic workload preparation and final L2 reference |
| `simulator/src/play.cpp` | PCAP preload, UDP multicast replay and pacing counters |
| `simulator/src/pitch.cpp` | PITCH encoding and bounded synthetic order state |
| `simulator/src/pcap.cpp` | Classic PCAP read/write and packet validation |
| `simulator/src/traffic.cpp` | Fixed-background, periodic-burst timing |
| `simulator/play_files/` | Generation recipes; generated PCAP/JSON files are ignored |
| `processors/cpp/` | C++ two-thread receiver, book, packet ring and gap queue |
| `processors/rust/` | Matching Rust implementation and locked dependencies |
| `benchmark/` | Build metadata, CPU placement, remote trials and timestamp probe |
| `configs/` | Portable CPU defaults and anonymized experimental hardware |
| `docs/experiments/` | Methods, all-attempt results, limits and JSON/CSV tables |
| `analysis/` | Ignored runtime reports and raw timestamp captures |
| `CMakeLists.txt` | C++ build and six CTest cases |

## Runtime path

```text
UDP socket → RX thread → bounded whole-packet SPSC ring → L2 thread
                  RX time                              decode and update
                                                           ↓
                                                bounded gap notification queue
```

The RX thread receives into a free packet slot; a full ring causes a new whole
packet to be dropped without overwriting unread data. The L2 thread owns order
and price-level state. Its reader detects bounded sequence gaps independent of
loss origin and publishes missing-message ranges to a separate queue. A recovery
consumer can use that handoff; no recovery worker or resend service is included.

Benchmark mode stops on loss and marks incomplete runs invalid. Continuous mode
can keep applying later input while recording loss and marking state incomplete.
See the [shared processor contract](../processors/README.md) for exact semantics.

The timestamp probe is an independent RX-only executable. It does not feed the
L2 processor. See [timestamp usage](usage.md#hardware-receive-timestamps) and
[the measured experiment](experiments/2026-11-12-hardware-rx.md).

Start with [usage](usage.md), then read the [experiment index](experiments/README.md).
