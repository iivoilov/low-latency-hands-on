# Benchmark configuration

[hardware.json](hardware.json) describes the two measured hardware/OS setups
using neutral CPU-based labels. They are observations from the experiments,
not configuration commands or exchange endpoints.

[baseline.json](baseline.json) supplies RX CPU 2, L2 CPU 3 and observer CPU 15
for the remote runner. That arrangement matched separate physical cores sharing
L3 on the measured Ryzen system. Override the masks on other hardware.
The default IDs must exist in the process's allowed CPU mask; raw processor
CLIs inherit OS placement and do not read this file.

UDP defaults are group `239.255.0.1`, port `30001`, loopback `127.0.0.1` and
TTL 1. Remote tests require your own addresses and verified multicast connectivity.
No machine connections or credentials are configured here. See
[usage](../docs/usage.md) and [remote automation](../benchmark/README.md).
