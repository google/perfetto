# Trace manifest format

A trace manifest (`perfetto_manifest`) is a JSON file placed inside a trace
archive (ZIP or TAR) which controls how
[Trace Processor](/docs/analysis/trace-processor.md)
and the Perfetto UI interpret the other files in the archive. It is a
general mechanism; the fields defined so far configure how multiple trace
files merge onto a single timeline: which machine each file belongs to, how
their clocks relate, and which clock the merged trace uses as its timeline.

This page is the normative reference for the format. For a task-oriented
guide to merging see
[Merging traces with Trace Processor](/docs/analysis/merging-traces.md); for
the underlying model see
[How trace merging works](/docs/concepts/merging-traces.md).

The format is stable: `version` 1 is the current (and only) version and will
remain supported. New capabilities are added as new fields within version 1;
fields a given Trace Processor version does not know are ignored.

## Why a manifest?

The [Perfetto UI's merge dialog](/docs/visualization/merging-traces.md)
configures a merge interactively, which is the right tool for a one-off
investigation. The manifest exists for when the merge is not a one-off:
tools and systems that produce several related traces per run, such as a
benchmarking framework tracing a client and a server, a test harness
recording one trace per device, or a pipeline capturing an app trace next
to a system trace.

Such a tool should not make every user reconstruct the merge configuration
in a dialog for every capture. It knows how its traces relate; the manifest
is how it writes that knowledge down. The tool emits the manifest alongside
the traces and packs everything into one archive, and that archive becomes
a single self-describing artifact: anyone can open it in the UI or in
`trace_processor` and get the correctly merged view with zero
configuration, today or years later.

The interactive dialog and the manifest are two faces of the same
mechanism: the dialog generates a manifest under the hood, and its "Copy
manifest" button is a convenient way to get a starting template. Since file
names, offsets and machine names usually vary per capture, tools generally
generate the manifest programmatically for each run and pack it into the
archive together with the trace files.

## Example

```json
{
  "perfetto_manifest": {
    "version": 1,
    "trace_time": {"clock": "BOOTTIME"},
    "files": [
      {"path": "phone.pftrace", "machine": {"name": "phone"}},
      {"path": "watch.pftrace", "machine": {"name": "watch"}},
      {
        "path": "app_log.json",
        "clocks": {
          "sync_to": {"file": "phone.pftrace", "clock": "BOOTTIME"},
          "offset_ns": 250000000
        }
      }
    ]
  }
}
```

## {#detection} Detection and placement

Trace Processor detects a manifest by content, not by file name: any file
whose contents (after leading whitespace) start with `{"perfetto_manifest"`
is treated as a manifest. By convention the file is named
`perfetto_manifest.json`, and that is the name the Perfetto UI uses when it
generates one, but any name works.

Placement rules:

- **Inside a ZIP or TAR archive**: position does not matter. Trace Processor
  always processes the manifest before any trace file, regardless of where it
  appears in the archive.
- **In a concatenated stream** (for example gzip members concatenated
  together): the manifest must come first. A manifest encountered after
  another trace file is rejected with an error.
- **At most one manifest** per merged input. A second one is an error.
- A standalone manifest (not inside an archive) parses successfully but has
  nothing to configure.

The manifest is applied in full before any trace file is parsed, so entries
may reference files in any order, including files that appear later in the
archive.

## {#schema} Top-level fields

The document is a JSON object with a single top-level `perfetto_manifest`
key, containing:

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `version` | integer | yes | Must be `1`. Any other value is rejected. |
| `trace_time` | object | no | Selects the clock of the merged timeline. See [trace_time](#trace-time). |
| `files` | array | no | Per-file configuration entries. See [files](#files). |

Files present in the archive but not listed in `files` are still imported;
they just get no overrides and follow the default merging rules described in
[How trace merging works](/docs/concepts/merging-traces.md).

## {#trace-time} trace_time

Selects the clock that becomes the merged trace's timeline (its "trace
time"). Without it, the first file to claim a trace-time clock wins, which
for Perfetto proto traces is normally `BOOTTIME`.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `clock` | string | yes | One of the [clock names](#clock-names). |
| `file` | string | no | Pins the clock to the machine of this file. Must match the `path` of an entry in `files`. |
| `machine` | string | no | When `file` is a multi-machine trace, names which of its machines owns the clock. Requires `file`. |

Every clock is scoped to a machine: `BOOTTIME` on the phone and `BOOTTIME`
on the watch are different clocks. `file` (and `machine`) select whose clock
becomes the timeline; without them the host machine's clock is used.

The selected clock id is recorded in the `metadata` table under the
`trace_time_clock_id` key.

## {#files} files

Each entry in the `files` array is an object:

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `path` | string | yes | The exact name of a file in the archive (for TAR/ZIP, the member path). |
| `machine` | object | no | Attributes the whole file to a named machine. Mutually exclusive with `machines`. |
| `machines` | array | no | Remaps a multi-machine trace's embedded machine ids to named machines. Mutually exclusive with `machine`. |
| `clocks` | object | no | Manually relates this file's clock to a clock in another file. See [clocks](#clocks). |

### {#machine} machine

```json
{"path": "watch.pftrace", "machine": {"name": "watch"}}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string (non-empty) | yes | The machine's name. |

Attributes every event in the file to a machine with the given name. Files
(or `machines` entries) using the same name share a single machine: their
processes, threads and CPUs are grouped together in the merged trace. Using
distinct names keeps each device's data separate.

`machine` is an object rather than a bare string so future per-machine
attributes can be added without a format change.

It is an error to use `machine` on a file that itself contains data from
several machines (a multi-machine proto trace recorded via
[traced_relay](/docs/deployment/multi-machine-architecture.md)); use
`machines` for those.

### {#machines} machines

```json
{"path": "relay.pftrace", "machines": [
  {"id": 0, "name": "host"},
  {"id": 1234, "name": "vm"}
]}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | integer in [0, 4294967295] | yes | A machine id embedded in the trace's packets. |
| `name` | string (non-empty) | yes | The name to give that machine. |

Renames the machines already embedded in a multi-machine trace. Every
embedded id which appears in the trace must be declared; a packet from an
undeclared id is an error. An entry with `id: 0` also becomes the file's
base machine. Names share the same namespace as `machine` names, so the same
name in two files merges them into one machine.

### {#clocks} clocks

Manually places this file on the shared timeline by relating one of its
clocks to a clock in another file. Use this when the automatic rules (shared
clock domains, `REALTIME` rendezvous) cannot place the file, or to apply a
known fixed offset.

```json
{
  "path": "app_log.json",
  "clocks": {
    "sync_to": {"file": "phone.pftrace", "clock": "BOOTTIME"},
    "offset_ns": 250000000
  }
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `clock` | string | no | Which of this file's own clocks to relate, as a [clock name](#clock-names). Omit for clockless files. |
| `machine` | string | no | When this file is multi-machine, names which of its machines owns the source clock. Required in that case. |
| `sync_to` | object | yes | The reference clock. See below. |
| `offset_ns` | integer | no (default 0) | Fixed offset between the two clocks: at a common instant, the source clock reads T when the reference clock reads T + `offset_ns`. A positive value therefore moves this file later on the reference's timeline. |

`sync_to` fields:

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `file` | string | yes | The reference file. Must match the `path` of an entry in `files`. |
| `machine` | string | no | When the reference file is multi-machine, names which of its declared machines owns the reference clock. Required in that case. A machine name alone (without `file`) is rejected as ambiguous. |
| `clock` | string | no | The reference clock, as a [clock name](#clock-names). When omitted, the reference is the file's own private per-file timeline (appropriate when the reference is itself a clockless file). |

The semantics of omitting `clock` matter:

- **`clock` present** (RELATE): the file keeps using its own clocks; the
  override only adds the missing relation between the named clock and the
  reference. Use this for internally-clocked traces (Perfetto proto,
  systrace, and so on).
- **`clock` omitted** (PIN): the file is treated as clockless. Its events
  are placed on its own private per-file timeline, which the override pins
  to the reference. Use this for formats without absolute clocks (Chrome
  JSON, Gecko, Instruments). Pinning a file which then turns out to emit
  its own clock snapshots is an error.

WARNING: a manual `offset_ns` that moves events before the start of the
merged timeline causes those events to be dropped, counted in the
`trace_sorter_negative_timestamp_dropped` stat. The Perfetto UI's merge
dialog reports this before opening.

## {#clock-names} Clock names

Wherever a clock name is expected, one of:

`REALTIME`, `REALTIME_COARSE`, `MONOTONIC`, `MONOTONIC_COARSE`,
`MONOTONIC_RAW`, `BOOTTIME`

These correspond to the builtin clocks in
[builtin_clock.proto](/protos/perfetto/common/builtin_clock.proto) and the
POSIX `clock_gettime` domains of the same names.

## {#sql} Effects on the SQL surface

After import, the manifest's effects are visible in the trace:

- Every named machine gets a row in the
  [`machine`](/docs/analysis/sql-tables.autogen#machine) table with its
  `name` set. Manifest machines get synthetic `raw_id` values starting at
  2^32, deliberately outside the 32-bit space of ids embedded in trace
  packets.
- `trace_time` sets the `trace_time_clock_id` key in the `metadata` table.
- Every `clocks` override is recorded as an edge in the `clock_snapshot`
  table, alongside the snapshots read from the traces themselves.
- Each input file has a row in the `trace_file` table; `stats` and
  `metadata` rows carry `machine_id` and `trace_id` columns identifying
  which machine and file they describe.

## {#errors} Errors

The manifest is validated up front; any violation fails the whole import
with an error prefixed by `perfetto_manifest:`. The conditions:

| Condition | Error |
|-----------|-------|
| Missing `version` | `missing required field: version` |
| `version` is not 1 | `unsupported version: N. Only version 1 is supported` |
| Unknown clock name | `unknown clock name: X. Use one of REALTIME, ...` |
| Second manifest in one input | `multiple perfetto_manifest files in archive` |
| Manifest after a trace file in a concatenated stream | `perfetto_manifest file must be the first trace file in the input` |
| `machine` and `machines` on the same entry | `machine and machines are mutually exclusive` |
| Empty machine name | `machine: name must be non-empty` |
| `machines` id outside [0, 4294967295] | `machines: id must be in [0, 4294967295]` |
| Packet from an embedded machine id not declared in `machines` | `undeclared machine id N` |
| `machine` on a file that is multi-machine | reported when the file's packets are parsed |
| `clocks` without `sync_to` | `clocks: a sync_to block is required` |
| `sync_to` without `file` | `clocks: sync_to.file is required` |
| `sync_to.file` not in `files` | `sync_to.file names unknown file 'X'. It must match the path of an entry in the files array` |
| `sync_to.machine` without `file` | a machine name alone is ambiguous, name the file too |
| Reference file is multi-machine but `sync_to.machine` missing | `'X' is a multi-machine trace; also name the machine` |
| `sync_to.machine` not declared by that file | `'X' is not a machine declared by file 'Y'` |
| This file is multi-machine but `clocks.machine` missing | `file 'X' is a multi-machine trace; name which machine the clock is on` |
| `offset_ns` not an integer / INT64_MIN | `offset_ns must be an integer` / `offset_ns is out of range` |
| Override on a file that is itself an archive or manifest | rejected |
| Pinning override on a file that emits clock snapshots | `clock overrides require the trace to use a single clock` |

The authoritative definition of the format is the reader in
[perfetto_manifest_reader.cc](/src/trace_processor/plugins/perfetto_manifest/perfetto_manifest_reader.cc)
and its test suite in
[trace_manifest/tests.py](/test/trace_processor/diff_tests/parser/trace_manifest/tests.py).

## Next steps

- [Merging traces with Trace Processor](/docs/analysis/merging-traces.md):
  building and querying merged archives.
- [Merging traces in the Perfetto UI](/docs/visualization/merging-traces.md):
  the interactive merge dialog, which generates this format for you.
- [How trace merging works](/docs/concepts/merging-traces.md): machines,
  the clock graph, and the automatic placement rules.
