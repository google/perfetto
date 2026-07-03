# How trace merging works

Trace Processor can open several trace files together and merge them onto a
single timeline: traces from different devices, from different processes on
one device, or in different formats entirely. This page explains the model
behind that merging: how events from independent files end up with
comparable timestamps, and how data stays attributed to the machine it came
from.

This is an explanation of the machinery. For task-oriented guides see
[Merging traces in the Perfetto UI](/docs/visualization/merging-traces.md)
and [Merging traces with Trace Processor](/docs/analysis/merging-traces.md).

## The problem

Two trace files recorded at the same time do not, in general, share a
timebase. Each file's timestamps are readings of some clock: `BOOTTIME` on
one phone, `MONOTONIC` inside a Chrome renderer, or no absolute clock at all
for formats like Chrome JSON. Clocks on different machines drift
independently, and even on one machine different clock domains (for example
`BOOTTIME` vs `REALTIME`) run at different origins.

Naively concatenating the files would place unrelated timestamps on one
axis. Merging instead requires answering, for every event: which clock was
this timestamp read from, and how does that clock relate to the clock the
merged trace uses as its timeline (the "trace time")?

## Clocks are scoped to a machine and a file

Within a single trace, Perfetto already models multiple clock domains and
converts between them using `ClockSnapshot` packets
(see [Clock synchronization](/docs/concepts/clock-sync.md)). Merging extends
the same model across files and machines: every clock is identified not just
by its domain but by which machine it belongs to and, where needed, which
file it was read in. `BOOTTIME` on the phone and `BOOTTIME` on the watch are
different clocks; so are the private timelines of two clockless JSON files.

All of these clocks live in one global clock graph. Nodes are clocks; edges
are known correspondences between two clocks, each stating "when clock A
read X, clock B read Y". Edges come from three sources:

- `ClockSnapshot` packets inside the traces themselves.
- Clock synchronization performed at recording time, such as the ping
  protocol used by [multi-machine recording](/docs/deployment/multi-machine-architecture.md).
- Entries in a [trace manifest](/docs/reference/perfetto-manifest.md),
  which let the user assert a correspondence (optionally with a fixed
  offset) that the traces do not contain.

To convert a timestamp, Trace Processor finds a path through this graph from
the source clock to the trace-time clock and applies each edge along the
way. Every edge, whatever its source, is recorded in the `clock_snapshot`
table, so the graph used for conversion is fully inspectable with SQL.

## Placing files that share no clock

When a file's clock has a snapshot path to trace time, that path is used and
nothing more is needed. Otherwise Trace Processor falls back through a
priority order:

1. **REALTIME rendezvous.** `REALTIME` (wall-clock time) is assumed to read
   the same value on every machine, since machines in practice synchronize
   it via NTP. If both the file's machine and the trace-time machine relate
   to `REALTIME`, the file is placed through it. This is what aligns two
   independently recorded phone traces at their true wall-clock positions.
2. **Same-domain assumption.** Two clocks in the same domain on different
   machines or files (for example two `BOOTTIME`s) are related at zero
   offset only if nothing better exists; similarly a file's private
   per-file clock can be pinned at zero offset. This is a guess, appropriate
   for files that came from the same boot of the same machine.
3. **Drop.** Two different real clock domains (say `BOOTTIME` here and
   `REALTIME` there) are never blindly equated. Events whose clock cannot be
   related to trace time are dropped and recorded in the trace's error stats
   (see [Checking the result](/docs/analysis/merging-traces.md#checking)).
   The fix is to record clock snapshots, or to assert the relation in a
   manifest.

NOTE: `REALTIME` rendezvous is only as accurate as the machines' wall
clocks. If NTP has not synchronized them, the traces will be offset by the
difference; a manifest `offset_ns` can correct for a known skew.

## Trace time and time bounds

One clock becomes the timeline of the merged trace. The first file to claim
a trace-time clock wins; since a manifest is always processed first, its
`trace_time` field takes precedence over anything the traces themselves
declare.

The merged trace's time bounds are the union of every (machine, file) pair's
recording window. Two traces recorded minutes apart therefore merge into a
long timeline with a cluster of activity at each end: "merging" places files
at their true relative time, it does not overlay them.

Timestamps that convert to before the start of trace time cannot be
represented and are dropped, again recorded in the trace's error stats. The
most common cause in merged traces is a manifest `offset_ns` that moves a
file too far.

## Machines

Merged data stays attributed to the machine it came from. A machine is one
device or OS instance: a phone, a server, a VM. In the trace model it is a
row in the [`machine`](/docs/analysis/sql-tables.autogen#machine) table, and
machine-scoped tables (`process`, `thread`, `cpu`, `sched`, and others)
carry a `machine_id` column referencing it. This is the same model used by
[live multi-machine recording](/docs/deployment/multi-machine-architecture.md);
merging populates it from several sources:

- **Ids embedded in the trace.** Packets recorded with a machine id (via
  traced_relay, or an SDK producer configured with
  `TracingInitArgs::machine_id`) carry it in `TracePacket.machine_id`, and
  each distinct id becomes a machine. A trace whose data is entirely from
  one such machine is "adopted" onto the host machine row, so a
  single-machine trace has exactly one machine rather than an empty host
  plus one remote.
- **Manifest declarations.** A manifest can attribute a whole file to a
  named machine, or rename the ids embedded in a multi-machine file. Named
  machines get synthetic `raw_id` values starting at 2^32, outside the
  32-bit embedded-id space; the same name used for several files means one
  shared machine.
- **`SystemInfo.machine_name`.** A producer can set a human-readable name in
  its `SystemInfo` packet, which fills the `machine.name` column. Nothing
  sets this automatically; without it (or a manifest name) UIs fall back to
  a numeric label such as "machine 2".

NOTE: `machine.id` (the table row id) is not stable across Perfetto
versions. Use `machine.raw_id` or `machine.name` to identify machines in
queries.

## Relationship to live multi-machine recording

Merging is one of three ways to get a trace spanning several machines; the
other two happen at recording time (relaying producers to a single `traced`,
or pre-stamping SDK producers with a machine id). The three approaches and
how to choose between them are covered in
[Multi-machine recording](/docs/learning-more/multi-machine-tracing.md).
They all produce the same model described above, and post-hoc merging can
also combine their outputs, for example merging two relay-recorded traces
from different host machines.

## Limitations

- An archive that itself contains multiple traces cannot be nested inside
  another merge: recursive synchronization is not supported. Merge the leaf
  files directly.
- Exporting a merged multi-machine trace to legacy JSON only exports the
  host machine's first trace.
- The UI builds merged inputs as an in-memory TAR, which limits individual
  files to sizes whose length encodes in 12 octal digits (about 8 GB) and
  member names to 99 characters.

## Next steps

- [Trace manifest format](/docs/reference/perfetto-manifest.md): the
  full reference for manual merge configuration.
- [Merging traces with Trace Processor](/docs/analysis/merging-traces.md):
  building merged archives and querying the result.
- [Clock synchronization](/docs/concepts/clock-sync.md): the single-trace
  clock model this builds on.
