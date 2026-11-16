# AF_XDP zero-copy capability check

Experiment date: 2026-11-13, Asia/Nicosia.

Ryzen's current Realtek receiver **rejects a forced AF_XDP zero-copy bind** with
`EOPNOTSUPP` (errno 95). The user ran the prepared probe as root and supplied
its output after the initial unprivileged attempts failed. UMEM and ring setup
succeeded before the bind failed. This agrees with the live kernel's advertised
`NETDEV_A_DEV_XDP_FEATURES = 0`, including an unset
`NETDEV_XDP_ACT_XSK_ZEROCOPY` bit. No zero-copy traffic or latency measurement
was completed.

## Observed environment

The receiver runs Debian kernel `6.12.116+deb13-amd64` on a Ryzen 9 9950X.
The PCI device ID is `10ec:8126`, using `r8169`. The interface is up at MTU
1500, with one RX queue and one TX queue. No XDP attachment appeared in the
interface inventory before or after the attempts.

## Attempts and results

1. An unprivileged `socket(AF_XDP, SOCK_RAW, 0)` failed with `EPERM` (errno 1).
2. `sudo -n true` reported that a password is required. Direct root SSH using
   the available key was rejected. The agent could not execute a privileged probe
   through those routes.
3. A standalone C++20 bind-only probe compiled successfully both locally and
   on the receiver with GCC, `-O2 -Wall -Wextra -Wpedantic -Wconversion`.
   Its default mode explicitly sets `XDP_ZEROCOPY`; a separate `--copy` mode
   sets `XDP_COPY`. Each invocation uses a new socket. The receiver invocation
   again failed at `socket(AF_XDP)` with `EPERM`, before UMEM registration or
   the forced bind. This is an access failure, not an observed unsupported-mode
   bind result.
4. An unprivileged read-only Generic Netlink `NETDEV_CMD_DEV_GET` query succeeded
   for the actual interface index. Its 64-bit XDP feature mask was zero:

   | Advertised capability | Value |
   | --- | ---: |
   | `NETDEV_XDP_ACT_BASIC` | false |
   | `NETDEV_XDP_ACT_REDIRECT` | false |
   | `NETDEV_XDP_ACT_XSK_ZEROCOPY` | false |

5. Following the access failures, the user ran the same prepared probe as root
   on the receiver and supplied this output:

   ```text
   interface=enp11s0 ifindex=2 queue=0 mode=XDP_ZEROCOPY
   setup=ok umem_bytes=262144 chunk_bytes=4096 ring_entries=64
   error=bind: Operation not supported errno=95
   ```

   This is the observed forced-zero-copy bind result. The privileged output is
   user-provided evidence; it was not captured through the agent's SSH session.

The query uses the constants verified in the receiver's installed
`linux/netdev.h`. Its raw feature attribute was `0000000000000000`.

## Interpretation and remaining verification

According to the [Linux 6.12 AF_XDP documentation](https://www.kernel.org/doc/html/v6.12/networking/af_xdp.html),
`XDP_ZEROCOPY` forces zero-copy or fails; omitting it permits copy fallback.
The [Linux 6.12 buffer-pool implementation](https://github.com/torvalds/linux/blob/v6.12/net/xdp/xsk_buff_pool.c)
rejects forced zero-copy when the required device features are absent. The
observed `EOPNOTSUPP` and zero live feature mask establish that forced zero-copy
is unavailable on this NIC/driver/kernel configuration. The installed kernel
is a Debian stable update rather than the exact upstream source revision linked
here. This result does not establish support on another driver or kernel.

The prepared probe uses queue 0, a page-aligned 256 KiB UMEM, 4096-byte chunks,
64-entry fill/completion/RX rings, and no TX ring. After a successful bind it
queries `XDP_OPTIONS` and requires the zero-copy bit to match the forced mode.
It attaches no XDP program, populates no fill descriptors, and sends or receives
no traffic. A successful copy-mode bind would establish socket setup only,
not packet-delivery correctness.

The forced-bind experiment is complete. The separate `--copy` control has not
been run. No packages were installed, XDP programs attached, or NIC/host settings
changed by the probe. Temporary probe files remain on the receiver. The raw
query, source, commands, unprivileged-probe result and user-reported privileged
output are retained under `analysis/xdp-zerocopy-2026-11-13/`.

## Ryzen recheck, 2026-11-16

A fresh read-only query on Debian 13 kernel `6.12.117+deb13-amd64` confirms
the same limitation. The receiver is still PCI `10ec:8126` (Realtek RTL8126),
using `r8169`, with one RX queue and one TX queue. The live Generic Netlink
XDP feature mask is again **0**: basic/native XDP, redirect and AF_XDP
zero-copy capability bits are all unset. The interface inventory is identical
before and after the query; no XDP program was attached or host setting changed.

This recheck did not repeat the privileged bind test. The earlier user-run
forced-zero-copy bind failed with `EOPNOTSUPP`; the new live capability query
establishes that zero-copy remains unavailable with the current configuration.
The upstream [Linux 6.12 r8169 driver](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/realtek/r8169_main.c)
also lacks `ndo_bpf` and `ndo_xsk_wakeup`. That source is supporting context,
not an exact source audit of the installed Debian module.

The precise conclusion is **no native XDP or AF_XDP zero-copy support with
the installed NIC/driver/kernel combination**. The
[kernel AF_XDP documentation](https://www.kernel.org/doc/html/v6.12/networking/af_xdp.html)
describes generic `XDP_SKB` copy mode for devices without native XDP support.
Its socket setup and packet delivery remain untested on ryzen; this mode
would still allocate SKBs and copy packet data into userspace UMEM. There is
no AF_XDP latency result for this receiver.

Raw query source, command and before/after inventories are retained under
`analysis/ryzen-xdp-recheck-2026-11-16/`. The portable result is
[2026-11-16-ryzen-xdp-capabilities.json](2026-11-16-ryzen-xdp-capabilities.json).

## EPYC bond0 follow-up

The same probe was subsequently compiled and executed as root on epyc's
`bond0`. This host runs kernel `6.1.0-54-amd64`, Debian build `6.1.190-1`, on an
EPYC 7742. The bond is active-backup at MTU 1500, with 16 RX/TX queues. Its
active member remained `eno2`, an Intel X550 port using `ixgbe`; `eno1` remained
the backup. Only queue 0 on the bond was probed in this step; the subsequent
direct-member probe is recorded below.

The source is identical to the ryzen probe: SHA-256
`2e301559aaad00b7d0ad42d5ea56b14d992607ea404ac068a1bbd7386bf550cb`.
GCC 12.2.0 compiled it with C++20,
`-O2 -Wall -Wextra -Wpedantic -Wconversion`, with no diagnostics. Each mode
was invoked once in a separate process; there were no retries.

| Forced mode | UMEM / ring setup | Bind result | `XDP_OPTIONS_ZEROCOPY` | Exit |
| --- | --- | --- | --- | ---: |
| `XDP_ZEROCOPY` | succeeded | `EOPNOTSUPP` (95) | not queried after failed bind | 1 |
| `XDP_COPY` | succeeded | succeeded | 0 | 0 |

The forced-zero-copy output was:

```text
interface=bond0 ifindex=4 queue=0 mode=XDP_ZEROCOPY
setup=ok umem_bytes=262144 chunk_bytes=4096 ring_entries=64
error=bind: Operation not supported errno=95
```

The separate copy control produced:

```text
interface=bond0 ifindex=4 queue=0 mode=XDP_COPY
setup=ok umem_bytes=262144 chunk_bytes=4096 ring_entries=64
bind=ok XDP_OPTIONS_ZEROCOPY=0
```

This establishes that the current bond interface rejects forced zero-copy while
accepting copy-mode socket setup. It does not establish a working copy-mode
packet path: no XDP redirect program was attached, fill descriptors were not
populated, and no traffic or latency was measured. The bond results alone do
not establish whether a direct bind to a physical X550 port would succeed.

The result agrees with the upstream Linux 6.1 source: the
[AF_XDP buffer-pool setup](https://github.com/torvalds/linux/blob/v6.1/net/xdp/xsk_buff_pool.c)
requires both `ndo_bpf` and `ndo_xsk_wakeup` for zero-copy, while the
[bond netdev operations](https://github.com/torvalds/linux/blob/v6.1/drivers/net/bonding/bond_main.c)
lack `ndo_xsk_wakeup`. This is source-based context rather than tracing of the
installed Debian kernel's exact failure branch.

Before/after link inventories for the bond and both members, the active-member
and membership readings, driver information, and probe binary checksum matched.
The probe attached no XDP program and changed no bond membership, addresses,
MTU, affinity, or host tuning. Raw build metadata, source, commands, exit codes,
standard output/error and inventories are retained under
`analysis/epyc-xdp-zerocopy-2026-11-13/`. Temporary probe files remain on the
host; no packages were installed.

## EPYC eno1 direct-member probe

At the user's request, the same compiled binary was run once as root against
`eno1`, without removing it from `bond0`. The binary SHA-256 matched the earlier
bond probe:
`7841142f810ef27e3f67d84330b5b39210d52e9d96ab2a6c0d68210a2d759a94`.
The interface was the backup member before the invocation.

```text
interface=eno1 ifindex=2 queue=0 mode=XDP_ZEROCOPY
setup=ok umem_bytes=262144 chunk_bytes=4096 ring_entries=64
bind=ok XDP_OPTIONS_ZEROCOPY=1
```

The process exited successfully (0), with no standard error. This confirms
forced zero-copy socket binding on the physical X550 port's queue 0 under the
installed `ixgbe` driver and kernel. Removing the port from the bond or unbinding
its kernel driver was unnecessary for this bind-only check. `eno2` was not
probed directly.

Before/after inventories matched exactly for all three link states, bond
membership and active member, bond IPv4 address, default route, `eno1` driver
information and the probe checksum. `eno2` remained active and `eno1` remained
the backup. No XDP program was attached and no network configuration was changed.

This remains a bind capability result. The probe supplied no fill descriptors
and did not test packet delivery, native XDP program attachment, multicast
arrival on the backup member, loss, throughput or latency. Raw records are
`eno1-zerocopy.json` and `eno1-inventory-before.json` / `eno1-inventory-after.json`
in `analysis/epyc-xdp-zerocopy-2026-11-13/`.

The subsequent [NIC-to-userspace timing attempt](2026-11-13-xdp-rx-timing.md)
found that native XDP program attachment fails at the installed ixgbe driver's
128-CPU guard on this 256-CPU host. The successful bind therefore does not yet
provide a working zero-copy receive measurement.

The [Debian 13 retest on 2026-11-16](2026-11-13-xdp-rx-timing.md#debian-13-retest-2026-11-16)
confirmed the same distinction on kernel 6.12.117: forced zero-copy bind succeeds
on the current backup X550 port `eno2`, fails on `bond0`, and native XDP program
attachment still fails at ixgbe's 128-CPU guard. No AF_XDP receive measurement
was obtained after the upgrade.
