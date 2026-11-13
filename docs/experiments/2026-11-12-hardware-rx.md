# Intel X550 hardware timestamps through an intact bond

12 November 2026. Ryzen sender → shared dual-EPYC 7742 receiver, Linux
6.1.0-54, ixgbe, existing 1 Gbps link. Hardware receive timestamps reached the
UDP socket through an active-backup bond. **Neither port left the bond.**
See [hardware and common methods](README.md).

## Three complete captures

P2 seed 43 uses 4 Mbps background, 20 Mbps peak and 5 ms bursts every 100 ms.
Each replay carries one million measured market updates plus initialization
and controls in **125,024 datagrams**. Achieved average was 4.80037–4.80038 Mbps.
All three captures received every expected packet with hardware and software
timestamps, zero detected loss/overflow/sequence errors, and no observed bond
failover. Faster profiles were not used after the
[reversal study](2026-11-12-reverse-direction.md) identified sender-queue drops.

The C++ probe is RX-only, with RX on CPU 2 and calibration on CPU 15, both in
NUMA node 0 with the NIC. It does not execute L2 updates or the processor's ring
consumer. Packet records are buffered and written after capture.

| Run | NIC → user p50 | p99 | p99.9 | Maximum |
| --- | ---: | ---: | ---: | ---: |
| 1 | 43.716 µs | 164.520 µs | 0.893 ms | 6.001 ms |
| 2 | 43.202 µs | 165.436 µs | 1.547 ms | 11.472 ms |
| 3 | 43.820 µs | 163.175 µs | 1.390 ms | 6.773 ms |

Median of the three per-run p99.9 values: **1.390 ms**; range **0.893–1.547 ms**.
This is not a pooled percentile. Median latency was similar across these runs;
the tail varied substantially. Packet samples number 125,024 per capture, not
one million independent wire observations.

The paired kernel software timestamps also split the receive path, in µs:

| Run | NIC → kernel p50 | NIC → kernel p99.9 | Kernel → user p50 | Kernel → user p99.9 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 24.560 | 195.011 | 18.490 | 841.554 |
| 2 | 23.927 | 217.021 | 18.451 | 1443.071 |
| 3 | 24.336 | 195.681 | 18.740 | 1033.859 |

Intervals are calculated per packet before calculating percentiles. Do not
add/subtract component percentiles. Linux software RX stamps are taken near
the driver-to-stack handoff. NIC-to-userspace includes DMA, interrupt moderation,
driver/NAPI, network/socket queues and scheduling; it is elapsed receive-path
delay rather than pure OS execution time.
[Linux timestamping](https://docs.kernel.org/networking/timestamping.html).

## How timestamps were obtained

A privileged helper enabled RX `all` on the active X550 member and temporarily
granted the probe's user group PHC read access. Traffic and socket membership
continued through the bond. The probe used `SO_TIMESTAMPING_NEW` with RX
hardware/raw-hardware and RX software/software reporting flags. `recvmsg`
returned hardware time in `ts[2]`, software time in `ts[0]`; the program read
MONOTONIC immediately after receiving and before parsing that ancillary data.
The PITCH payload was unchanged.

The initial direct-bond helper attempt omitted
`HWTSTAMP_FLAG_BONDED_PHC_INDEX` and failed with `EOPNOTSUPP`. Its attempt to
restore through the bond also failed for that reason; physical settings and
PHC permissions were restored. The three measured captures used direct-member
configuration. The helper now preserves the bonding flag and retains a setup
error if cleanup also fails. Regression tests and a later one-second live
bond-level session verified corrected setup and restoration.
[Linux 6.1 bond ioctl implementation](https://raw.githubusercontent.com/torvalds/linux/v6.1/drivers/net/bonding/bond_main.c).

## Clock correlation and uncertainty

A separate thread chose the narrowest of 25 MONOTONIC/PHC/MONOTONIC read
brackets approximately every 100 ms. There were 426 anchors per run, before
and after traffic. Analysis interpolated PHC offsets between anchors, preserving
nanosecond differences with integer cancellation before floating-point math.
There was no extrapolation, missing hardware stamp, out-of-range packet or
negative NIC-to-userspace estimate.

Maximum read bracket half-width was **2.786 µs**. Maximum leave-one-out
interpolation residuals were **55–61 ns**. Software REALTIME-to-MONOTONIC
read brackets had a consistent 40 ns intersection. Small residuals are diagnostic;
they do not certify a total error bound. Interpolation assumes smooth clock drift.
No clock was adjusted and no cross-host synchronization was required.

The exact PHY/MAC wire reference and latency correction remain uncalibrated.
These are NIC-timestamp-to-userspace measurements. Wire-to-L2 is unmeasured
by this independent probe. The server ran other workloads and pins were not
exclusive. A post-capture read-only check found `rx-usecs=1`, selecting dynamic
interrupt moderation in ixgbe; there was no moderation-disabled or quiet-host
control. These factors are possible contributors, not isolated causes.
[Intel ixgbe documentation](https://github.com/intel/ethernet-linux-ixgbe/blob/main/README).

## Restoration, data and reproduction

Both the measured member-level session and corrected bond-level test restored
successfully. Final checks confirmed RX timestamps disabled, TX off, both PHCs
back at root ownership/mode 0600, unchanged bond membership and the same active
port. No link, route, IRQ, governor, queue policy or clock setting was changed.

[JSON measurements/clock diagnostics](2026-11-12-hardware-rx-results.json) ·
[CSV](2026-11-12-hardware-rx-results.csv) ·
[Step-by-step hardware capture](../usage.md#hardware-receive-timestamps) ·
[Remote capture automation](../../benchmark/README.md#automated-hardware-captures).

Raw packet/clock CSVs and operational session records are not distributed.
The published tables retain all three attempts and exact numeric summaries,
with environment-specific identifiers omitted.
