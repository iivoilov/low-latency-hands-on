# AF_XDP NIC-to-userspace timing attempt

Experiment date: 2026-11-13, Asia/Nicosia. The requested measurement could not
start on the installed kernel/driver. A native XDP attachment on the physical
X550 backup port fails with `ENOMEM` because this ixgbe build rejects systems
with more than 128 possible CPUs. The host has 256 possible CPUs. No latency
samples were collected, and no AF_XDP percentiles are available.

Retested on 2026-11-16 after the Debian 13 upgrade: native attachment remains
blocked by the same guard on kernel 6.12.117+deb13-amd64. See the
[upgrade retest](#debian-13-retest-2026-11-16) below; the original observations
and installed-driver evidence remain historical.

## Intended measurement and existing evidence

The intended comparison is with the three earlier P2
[NIC-timestamp-to-userspace captures](2026-11-12-hardware-rx.md): 125,024
datagrams per capture, all hardware-stamped, with no detected loss. Those
captures used a UDP socket through the intact active-backup bond. The new
endpoint would be the userspace observation of an AF_XDP RX descriptor, after
acquire of the producer index, with hardware timestamps correlated to
`CLOCK_MONOTONIC`. It would still be an RX-only measurement, not L2 processing.

The [preceding bind probe](2026-11-13-xdp-zerocopy.md) succeeded directly on
physical port `eno1`, queue 0, and returned `XDP_OPTIONS_ZEROCOPY=1`. The same
forced zero-copy bind on `bond0` failed with `EOPNOTSUPP`. A zero-copy socket
bind does not establish native XDP program attachment or packet delivery.
`XDP_OPTIONS_ZEROCOPY` reports the mode; it does not supply a hardware timestamp.

## Host and readiness checks

The host runs Debian kernel `6.1.0-54-amd64`, build `6.1.190-1`, with `ixgbe`
on the X550. `/sys/devices/system/cpu/possible` and `online` both report
`0-255`. The bond is active-backup with `eno2` active and `eno1` the backup.
Both X550 ports advertise hardware RX timestamps with an `all` filter.
Large receive offload on `eno1` was already off; no offload setting was changed.

A minimal two-instruction BPF program returns `XDP_PASS` for every packet.
The readiness script checked that no XDP program was already attached to
`eno1` in generic, native, or hardware mode, loaded this program, then attempted
one native attachment with `XDP_FLAGS_DRV_MODE | XDP_FLAGS_UPDATE_IF_NOEXIST`.
There was no generic-mode fallback and no retry. The script includes removal
of its own program if attachment succeeds, conditional on the expected program
file descriptor.

| Check | Result |
| --- | --- |
| BPF program load / verification | succeeded |
| Native attach to `eno1` | failed: `ENOMEM` (12) |
| Before/after bond, link, address and default-route inventories | identical |
| Final XDP attachment on bond or either member | none |
| AF_XDP traffic captures / latency samples | 0 / 0 |

The readiness script exited 0 after recording the attempted attachment and
checking state. That is not a successful receive test or timing measurement.

## Why attachment fails

The matching upstream stable ixgbe source defines `IXGBE_MAX_XDP_QS` as 64 and
returns `-ENOMEM` when `nr_cpu_ids > IXGBE_MAX_XDP_QS * 2`. The installed module
was also inspected read-only: its `ixgbe_xdp` entry point compares `nr_cpu_ids`
with `0x80` and branches to a return of `0xfffffff4` (`-12`) when exceeded.
This establishes the CPU-count guard in the actual driver file rather than
attributing the error to ordinary memory pressure. See
[stable ixgbe setup](https://github.com/gregkh/linux/blob/v6.1.190/drivers/net/ethernet/intel/ixgbe/ixgbe_main.c)
and [queue definitions](https://github.com/gregkh/linux/blob/v6.1.190/drivers/net/ethernet/intel/ixgbe/ixgbe.h).

Pinning this process does not change `nr_cpu_ids`. Removing a port from the
bond would not remove this driver guard. No CPU offlining, boot changes, kernel
upgrade, module replacement, or bond removal was attempted.

## Hardware timestamp path still needs validation

The installed kernel headers lack the standard XDP RX metadata API, and neither
the live kernel nor ixgbe BTF contains `bpf_xdp_metadata_rx_timestamp`. Modern
[XDP RX metadata documentation](https://docs.kernel.org/networking/xdp-rx-metadata.html)
describes how a supporting driver and XDP program can carry a hardware timestamp
into AF_XDP metadata; that path is unavailable on this installed build.

There is a potential driver-specific alternative. In upstream 6.1.190,
[X550 timestamp setup](https://github.com/gregkh/linux/blob/v6.1.190/drivers/net/ethernet/intel/ixgbe/ixgbe_ptp.c)
enables an eight-byte hardware timestamp trailer, normally consumed by the
SKB timestamp path. The
[zero-copy RX loop](https://github.com/gregkh/linux/blob/v6.1.190/drivers/net/ethernet/intel/ixgbe/ixgbe_xsk.c)
passes the buffer to XDP without that SKB conversion. This suggests the trailer
could be available in the AF_XDP buffer, but it was not verified with traffic.
Its raw counter requires conversion consistent with the driver's timecounter;
it must not simply be assumed to contain PHC epoch nanoseconds.

A kernel/driver supporting native XDP on this CPU configuration is needed before
a receive trial. Even after that, hardware timestamp extraction and clock
correlation must be validated before reporting the requested latency. No claim
is made that an arbitrary newer kernel resolves both requirements.

## Artifacts and state

Raw evidence is retained under `analysis/epyc-xdp-timestamps-2026-11-13/`:
the readiness script and result, final live metadata/interface checks, installed
driver disassembly, and the relevant upstream 6.1.190 source files. The initial
disassembly of the inlined `ixgbe_xdp_setup` function produced no instructions;
the subsequent `ixgbe_xdp` disassembly contains the actual guard.

No packets were sent for this attempt. Hardware timestamp generation was not
enabled, PHC permissions were not changed, and no clock was adjusted. No packages
were installed. The successful BPF load was temporary; its descriptor was closed
after the rejected attachment. Bond membership, active port, addresses, routes
and NIC configuration remain unchanged.

## Debian 13 retest, 2026-11-16

At the user's request, the readiness test was repeated on epyc after its
Debian 12 to Debian 13 upgrade. The host now reports Debian 13 (trixie), kernel
6.12.117+deb13-amd64 and 256 possible/online CPUs (`0-255`). The X550 ports still
use ixgbe. Bond0 remains active-backup, now with `eno1` active and `eno2` backup;
the new test selected `eno2` after checking its current backup state.

The two-instruction `XDP_PASS` program loaded and passed verification. Its one
native attachment attempt failed with `ENOMEM` (12), just as before the upgrade.
There was no attachment retry or generic-mode fallback. The script refused any
existing XDP attachment, used `UPDATE_IF_NOEXIST`, and included conditional
cleanup restricted to its own program. Final queries returned zero program IDs
in generic, native and hardware modes on the bond and both physical ports.

Read-only inspection confirms that the installed `ixgbe_xdp` still compares
`nr_cpu_ids` with `0x80` and branches above that value to return `0xfffffff4`
(`-ENOMEM`). The loaded module's GNU ELF build ID matches the inspected module
file. The failure is therefore still explained by the 128-CPU guard, rather than
ordinary memory exhaustion. The same guard remains in
[upstream Linux 6.12](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/intel/ixgbe/ixgbe_main.c#L9623).

The unchanged bind-only C++ probe was compiled with GCC 14.2.0, C++20,
`-O2 -Wall -Wextra -Wpedantic -Wconversion`, with no diagnostics. Each control
ran once, on queue 0 with 256 KiB UMEM, 4096-byte chunks and 64-entry rings:

| Check | Result |
| --- | --- |
| `eno2`, forced zero-copy bind | succeeded; `XDP_OPTIONS_ZEROCOPY=1` |
| `bond0`, forced zero-copy bind | failed; `EOPNOTSUPP` (95) |
| `bond0`, forced copy bind | succeeded; `XDP_OPTIONS_ZEROCOPY=0` |
| Native `XDP_PASS` attachment on `eno2` | failed; `ENOMEM` (12) |
| AF_XDP traffic packets / latency samples | 0 / 0 |

The readiness and bind-only stages both have identical before/after link,
bond-membership/active-port, bond-address, default-route and MTU inventories.
No XDP program remains attached. No packages were installed, host/NIC settings
changed, CPUs offlined, ports removed from the bond, or timestamp generation
enabled. There was no feed transmission, AF_XDP capture or L2 benchmark.
Socket bind success remains distinct from a working native-XDP receive path.

The first read-only module-inspection script stopped because this build exposes
no `/sys/module/ixgbe/srcversion`. Its failed log is preserved. The corrected
inspection used the live/file ELF build IDs, then completed the bind controls;
the native attachment itself was attempted only once.

Portable [results](2026-11-16-xdp-recheck-results.json) record all outcomes and
restoration checks. Raw scripts, commands, verifier output, before/after
inventories, module information/build IDs and disassembly are retained under
`analysis/epyc-xdp-recheck-2026-11-16/`. The bind probe remains in a new
isolated remote build directory. The Debian upgrade has not removed this
particular native-XDP blocker; hardware timestamp delivery through AF_XDP also
remains unverified.
