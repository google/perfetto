# Multi-machine architecture

Perfetto can record a single trace that spans more than one operating system
image — for example, a host and one or more virtual-machine guests, an SoC
and a companion processor, or a fleet of test machines driving a shared
workload. The result is one timeline in which causality across machines is
visible and queryable, instead of one trace per machine that has to be
correlated by hand.

This page explains *what* multi-machine tracing is and *how* the pieces fit
together. For the step-by-step setup, see
[Multi-machine recording](/docs/learning-more/multi-machine-tracing.md).

## Problem statement

The standard [service model](/docs/concepts/service-model.md) assumes that
all producers, the `traced` service, and the consumer share one OS image:
they reach `traced` through a local UNIX socket, agree on PIDs, and observe
the same `CLOCK_BOOTTIME`.

That assumption breaks as soon as a producer lives on a different kernel.
There is no shared filesystem socket. PID namespaces are independent. Boot
clocks start at different points and drift independently of each other.
Running a separate `traced` on every machine and stitching the resulting
traces together after the fact is possible but fragile, especially for
anything timing-sensitive (e.g. cross-machine scheduling or RPC latency).

Multi-machine tracing solves this without duplicating buffers or consumer
machinery on every machine.

## Architecture

Exactly one machine in the setup runs `traced` (the "host"). Every other
machine runs `traced_relay`, which forwards the producer-side IPC to the
host:

```
   Remote machine                            Host machine
  ┌────────────────────────┐               ┌────────────────────────────┐
  │ traced_probes          │               │  traced --enable-relay-    │
  │ + other producers      │               │          endpoint          │
  │        │               │               │           ▲                │
  │        ▼ (local IPC)   │   TCP/vsock   │           │ (local IPC)    │
  │  traced_relay  ────────┼──────────────►│  relay endpoint            │
  └────────────────────────┘               │           ▲                │
                                           │           │                │
                                           │   traced_probes / other    │
                                           │   local producers          │
                                           │           ▲                │
                                           │           │ (consumer IPC) │
                                           │      perfetto cmdline      │
                                           └────────────────────────────┘
```

`traced_relay` is intentionally thin: it accepts producer connections on the
local producer socket, exchanges a small amount of metadata with the host
(see below), and then proxies producer IPC frames over TCP or vsock. It does
not buffer trace data, does not parse trace packets, and does not implement
any consumer-side functionality.

The consumer (`perfetto` cmdline or the UI's WebSocket bridge) only ever
talks to the host's `traced`. Trace configuration, buffer ownership, and
final read-back stay on a single machine.

## Machine identity

When `traced_relay` first connects to the host it sends a `SetPeerIdentity`
message containing a `machine_id_hint` — on Linux this is derived from
`/proc/sys/kernel/random/boot_id` when available, or a hash of `uname(2)`
plus a bootup-timestamp source as a fallback. The hint is stable across
reconnects of the same kernel, but distinct between different kernels.

The host's `traced` maps each unique hint to a small integer `MachineId`
and stamps every `TracePacket` arriving from that relay with it (the
`machine_id` field on `TracePacket`). At import time, [Trace Processor]
materialises one row per machine in the `machine` table:

| Column | Description |
| ------ | ----------- |
| `id` | Trace-Processor-assigned machine ID. Always `0` for the host. |
| `raw_id` | The raw machine identifier from the trace packet (`0` for the host, non-zero for remote machines). |
| `sysname`, `release`, `version`, `arch` | `uname(2)` fields for the machine. |
| `num_cpus` | CPU count visible to that kernel. |
| `system_ram_bytes`, `system_ram_gb` | Total RAM. |
| `android_build_fingerprint`, `android_device_manufacturer`, `android_sdk_version` | Populated only for Android machines. |

Tables that have a per-CPU or per-thread dimension (`thread`, `cpu`,
`gpu_counter_track`, etc.) carry a nullable `machine_id` so cross-machine
data can be sliced by SQL. UI support for per-machine tracks is still
maturing, so `machine_id` joins remain the most reliable way to answer
cross-machine questions today.

## Clock synchronisation across machines

Each remote machine has its own `CLOCK_BOOTTIME`, so timestamps written by
its producers cannot be compared directly to host timestamps. `traced_relay`
runs a lightweight ping protocol against the host's relay endpoint, sending
and receiving timestamped messages to estimate the per-machine clock offset
and round-trip time. The host periodically emits the resulting offsets as
`ClockSnapshot` packets in the trace.

From there everything reuses the existing single-machine machinery
described in [Clock Synchronization](/docs/concepts/clock-sync.md): Trace
Processor folds the cross-machine offsets into the same clock graph it
already builds for `CLOCK_REALTIME`, `CLOCK_MONOTONIC`, etc., and resolves
every event to a single global trace clock at import. There is nothing
extra a data source has to do.

## {#data-source-dispatch} Data source dispatch

By default `traced` only dispatches data sources to producers on the host
machine. To collect data from remote machines, the consumer's
`TraceConfig` must opt in, either globally with `trace_all_machines: true`
or per-data-source with `DataSource.machine_name_filter`. Without one of
these, `traced_probes` on the remote machine still registers and shows up
as a row in the `machine` table, but is never assigned the requested data
sources, so no events flow from it.

`trace_all_machines` was introduced in v54; earlier versions matched all
machines by default. The remote-side machine name comes from the
`PERFETTO_MACHINE_NAME` env var when `traced_relay` is started, falling
back to `uname -s`. The literal name `"host"` is a synonym for the
machine running `traced`.

Producers on a single kernel cannot stand in for "two machines" even for
testing. The two `traced_probes` instances would race over the same
`/sys/kernel/tracing/` ring buffers, and per-CPU events would be
partitioned arbitrarily between the two `machine_id`s — the trace looks
valid but is silently torn. Multi-machine setups need two kernels (two
machines, host plus a VM, separate containers with their own kernel
namespaces, etc.).

## Limitations and constraints

* `traced_relay` cannot run on the same machine as `traced` — both bind the
  local producer socket. Each machine in the setup runs *either* `traced`
  (the host) *or* `traced_relay` (every other machine).
* Every remote machine must have a network path to the host's relay
  endpoint, on TCP or vsock.
* Cross-machine clock alignment is only as good as the ping protocol's
  measurement of the offset; a roughly-aligned wall clock (NTP or
  similar) helps the first snapshots but is not strictly required.
* UI per-machine track rendering is still maturing. SQL on the `machine`
  table and `machine_id` columns is the authoritative way to slice
  cross-machine data today.

## Next steps

* [Multi-machine recording](/docs/learning-more/multi-machine-tracing.md) —
  step-by-step walk-through of recording a multi-machine trace between two
  Linux hosts.
* [Clock Synchronization](/docs/concepts/clock-sync.md) — the single-machine
  clock-sync graph that the cross-machine offsets fold into at import.
* [`machine` table reference](/docs/analysis/sql-tables.autogen#machine) —
  full schema of the table populated from `SetPeerIdentity`.

[Trace Processor]: /docs/analysis/trace-processor.md
