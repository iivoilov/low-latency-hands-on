# Market-data references

Sources checked on 2026-11-12. Protocol examples, sample programs, historical
datasets, and live multicast connectivity are different resources.

## BATS / Cboe

[U.S. Equities/Options Multicast PITCH specification](https://www.cboe.com/document/tech-spec/document/technical-specifications/cboe-titanium-u.s.-equitiesoptions-multicast-pitch-specification)
documents BYX/BZX/EDGA/EDGX feeds, binary fields, sequences, and packet examples.
It links an official [multicast test program ZIP](https://cdn.cboe.com/resources/membership/mcast_pitch.zip),
a connectivity example rather than a historical capture. Its source has not
been incorporated here.

[U.S. Equities PITCH historical data](https://datashop.cboe.com/cboe-us-equities-pitch)
is an exchange/unit archive product. Its `.dat.gz` files need a separate reader.

[Cboe Europe Trades and Quotes](https://datashop.cboe.com/cboe-europe-equities-trades-and-quotes-data)
offers a [sample ZIP](https://datashop.cboe.com/download/sample/197). Its contents
were not inspected; do not assume it contains U.S. PITCH UDP PCAP files.

## ICE

[ICE Technology ISV: market data](https://status.ice.com/fixed-income-data-services/access-and-delivery/connectivity-and-feeds/technology-isv)
confirms iMpact multicast updates/snapshots, sequence numbers, and TCP services
for definitions/replay. Development/production API access requires ICE approval.
No public ICE iMpact packet capture or complete message specification was verified
in this search. Obtain the applicable specification and samples through ICE
integration support before implementing its encoder. ICE is not implemented.

## Processor coverage

As of 2026-11-14, both C++ and Rust processors decode every U.S. equities
multicast message in revision 2.41.87, build L2, maintain trade history and
generate L1 bid/ask/last decisions. See the [coverage table and semantics](pitch-l1.md).
Options-specific multicast messages remain outside this implementation. Optional
Cboe GRP and spin services were added on 2026-11-15; see [recovery](recovery.md).
The generator remains the deterministic subset described below.

## Simulator subset

The generator uses the BYX/EDGA Time variant. Encoded layouts:

| Component | Type | Bytes |
| --- | --- | --- |
| Sequenced Unit Header | — | 8 |
| Time | `0x20` | 6 |
| Unit Clear | `0x97` | 6 |
| Add Order, long | `0x21` | 34 |
| Order Executed, equities | `0x23` | 26 |
| Reduce Size, long | `0x25` | 18 |
| Modify Order, long | `0x27` | 27 |
| Delete Order | `0x29` | 14 |

Integers are little-endian; long prices have four decimal places. Sequences
advance per message, wrapping UINT32_MAX to 1. Time is seconds since midnight
Eastern; event offsets are nanoseconds. Symbols are six ASCII bytes space padded.

Workload choices: Time at startup/second boundaries, initial Unit Clear, and
one configurable lab unit. Symbol partitioning is not applied. The preparer seeds
both sides at ten one-cent levels with three orders at each level by default.
Updates modify price/size, reduce, execute, and delete; dead orders are refilled
with new IDs. Total state is bounded to 4096 slots. Prices, IDs, order flow, and
byte-based traffic schedules are synthetic, not measured exchange behavior.
A final heartbeat marks the end of the traffic budget.

Other generated order types, trading status, auctions, and redundant feeds are
absent. The separate `serve_feed` service adds recovery and post-file heartbeats.
BZX/EDGX extended Time and Time Reference are not generated. Replay preserves any well-framed payload without decoding semantics.
This is a laboratory subset, not a complete exchange emulator.

Default processors detect loss and either stop or continue with explicit
state validity. Optional `--grp` recovery buffers later messages, requests
missing ranges, and uses a configured spin service for snapshot fallback.
`serve_feed` supplies the corresponding laboratory services from a complete PCAP.
The baseline reader's gap notification queue remains available. See
[recovery behavior, limits and verification](recovery.md).
