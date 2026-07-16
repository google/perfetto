# Merging traces with Trace Processor

Trace Processor can import several trace files as one merged trace: events
from every file end up on a single timeline, with their processes, threads
and CPUs kept attributed to the machine they came from. This page shows how
to do it from the command line and in scripted or CI setups. For the
interactive equivalent see
[Merging traces in the Perfetto UI](/docs/visualization/merging-traces.md);
for how the merging actually works see
[How trace merging works](/docs/concepts/merging-traces.md).

## The model: one archive in, one trace out

`trace_processor` takes a single trace file argument. To merge, you pass one
archive (ZIP or TAR) containing the files to merge. The `util merge`
subcommand builds such an archive:

```bash
trace_processor util merge -o merged.tar trace_a.pftrace trace_b.pftrace
trace_processor merged.tar
```

The archive is plain TAR, so `tar cf merged.tar trace_a.pftrace ...` (or any
ZIP tool) works too; `util merge` is just a convenience helper, described
[below](#merge-util).

Everything that accepts a normal trace accepts such an archive: the
interactive shell, `-q` batch queries,
[httpd mode](/docs/analysis/trace-processor.md#subcommands) serving the UI,
and the [C++](/docs/analysis/trace-processor.md#embedding) and
[Python](/docs/analysis/trace-processor-python.md) APIs, which stream the
archive bytes like any other trace.

How the files line up on the timeline is decided by clocks. Three setups
cover most cases, in increasing order of configuration needed.

## {#no-config} Merging that needs no configuration

No extra configuration is needed when Trace Processor can already relate the
files' clocks:

- **Traces from the same device.** Files recorded during the same boot share
  clock domains (for example `BOOTTIME`), and files with `ClockSnapshot`
  packets relate their domains explicitly.
- **Traces from different devices with wall-clock sync.** `REALTIME` is
  assumed to be the same on every machine (in practice: NTP), so two phone
  traces recorded at the same time align at their true wall-clock positions
  automatically.
- **Traces pre-stamped with a machine id.** An SDK producer initialized with
  a machine id tags every packet it writes, so merged files keep their data
  on separate machines with no manifest at all:

  ```c++
  perfetto::TracingInitArgs args;
  args.backends = perfetto::kInProcessBackend;
  args.machine_id = 42;  // Non-zero, unique per machine.
  perfetto::Tracing::Initialize(args);
  ```

  The C SDK equivalent is `PerfettoProducerBackendInitArgsSetMachineId()`.
  A producer can additionally set `SystemInfo.machine_name` to give the
  machine a human-readable name in the merged trace.

If neither a shared clock domain nor `REALTIME` can place a file, its events
are dropped rather than guessed (see
[checking the result](#checking) below); that is when you need a manifest.

## {#manifest} Merging with a trace manifest

A [trace manifest](/docs/reference/perfetto-manifest.md)
(`perfetto_manifest`) is a JSON file added to the archive which controls
how Trace Processor interprets the files in it; for merging, it names
machines, remaps embedded machine ids, and manually relates clocks. Trace
Processor always processes it before the trace files, wherever it sits in
the archive.

The manifest is what makes merging automatable. If you are building a tool
that produces several traces per run (a benchmarking framework tracing a
client and a server, a test harness recording one trace per device), your
tool knows how its traces relate; write that down in a manifest and pack
one archive. The archive is then a single self-describing artifact: your
users open it in the UI or in `trace_processor` and get the correctly
merged view every time, with no per-capture configuration.

How your tool builds the archive does not matter: it is ordinary TAR or
ZIP, so any tar/zip library works. The
[`util merge` helper](#merge-util) below is just a convenience for doing
the same thing from the command line, with some validation on top.

### Keeping two devices' data separate

By default, two same-device-looking traces merge onto one machine. Naming
machines keeps each file's processes, threads and CPUs grouped separately:

```json
{
  "perfetto_manifest": {
    "version": 1,
    "files": [
      {"path": "device_a.pftrace", "machine": {"name": "device-a"}},
      {"path": "device_b.pftrace", "machine": {"name": "device-b"}}
    ]
  }
}
```

```bash
trace_processor util merge -o merged.tar --manifest manifest.json \
    device_a.pftrace device_b.pftrace
trace_processor merged.tar
```

Files given the same machine name share one machine; distinct names create
distinct `machine` table rows.

### Placing a clockless trace against a system trace

Formats without absolute clocks (Chrome JSON, Gecko, Instruments) have no
way to line up with a system trace on their own. A `clocks` entry pins the
file to another file's clock, optionally at a fixed offset:

```json
{
  "perfetto_manifest": {
    "version": 1,
    "trace_time": {"clock": "BOOTTIME"},
    "files": [
      {"path": "system_trace.pftrace"},
      {
        "path": "app_trace.json",
        "clocks": {
          "sync_to": {"file": "system_trace.pftrace", "clock": "BOOTTIME"},
          "offset_ns": 100000000
        }
      }
    ]
  }
}
```

`offset_ns` means: at a common instant, the source file's clock reads T when
the reference reads T + `offset_ns`, so a positive value moves the file
later on the timeline. Note that `sync_to.file` must itself appear as an
entry in `files`.

### Renaming machines in a multi-machine trace

A trace recorded with
[traced_relay](/docs/learning-more/multi-machine-tracing.md) already
contains several machines, identified by numeric ids. `machines` gives them
readable names (every embedded id must be listed):

```json
{
  "perfetto_manifest": {
    "version": 1,
    "files": [
      {"path": "relay_capture.pftrace", "machines": [
        {"id": 0, "name": "host"},
        {"id": 1234, "name": "vm"}
      ]}
    ]
  }
}
```

The full grammar, defaults and error catalog are in
[the trace manifest reference](/docs/reference/perfetto-manifest.md).

TIP: the Perfetto UI's "Open multiple trace files" dialog generates this
format interactively: configure the merge there, then use "Copy manifest" or
"Download .tar" to bootstrap a scripted setup.

### {#merge-util} Packing with util merge

`util merge` is optional: since the archive is ordinary TAR (or ZIP), you
can just as well tar/zip the trace files and manifest together yourself.
The helper takes care of the archive layout (member naming, and including
the manifest as `perfetto_manifest.json` whatever the file passed via
`--manifest` is called) and runs sanity checks on the result, warning if
the archive would not merge cleanly. `--strict` turns the warnings into a
failing exit code, useful in CI; `--no-validate` skips the checks.

## {#checking} Checking the result

The merged trace exposes what happened during the merge:

```sql
-- The machines in the merged trace and how much data each has.
SELECT
  m.name,
  m.raw_id,
  (SELECT COUNT(*) FROM thread t WHERE t.machine_id = m.id) AS threads
FROM machine m;

-- The input files and the order they were processed in.
SELECT name, trace_type, size FROM trace_file;

-- Anything dropped or misaligned during the merge. An empty result means
-- every event was placed on the timeline.
SELECT name, value, machine_id, trace_id
FROM stats
WHERE severity = 'error' AND value > 0;
```

The stats to watch for merges: `clock_sync_unrelatable_clock_domains` and
`clock_sync_failure_no_path` count events whose clock could not be related
to the timeline (record clock snapshots or add a manifest `clocks` entry);
`trace_sorter_negative_timestamp_dropped` counts events an `offset_ns`
moved before the start of the timeline.

Per-file metadata is available via the `metadata` table's `trace_id` column
or, at a higher level, the `_metadata_by_trace` view in the
`traceinfo.trace` stdlib module.

## Interop notes

- **Android bugreports**: `bugreport.zip` files already open as archives;
  Trace Processor extracts and merges the traces inside.
- **traceconv bundle**: the TAR produced by
  [`traceconv bundle`](/docs/quickstart/traceconv.md) (trace plus symbols)
  is the same archive mechanism.
- **Python `BatchTraceProcessor`** does not merge: it loads N traces into N
  independent instances for parallel querying. To merge, pass one archive
  to a single `TraceProcessor` instance.
- Archives containing archives cannot be merged recursively; merge the leaf
  files directly.
- **Hidden files are ignored**: any archive entry whose name has a path
  component starting with a `.` is skipped and never parsed as a trace. This
  covers the metadata that archiving tools add automatically — most notably the
  AppleDouble resource-fork files (`._foo`) and `.DS_Store` entries that macOS
  `tar` and Finder-created ZIPs sprinkle next to the real files. As a result a
  `.tar`/`.zip` built on macOS loads without a spurious "unknown trace type"
  error. If you deliberately want a dot-prefixed file to be parsed, rename it so
  no path component starts with a `.`.

## Next steps

- [Trace manifest format](/docs/reference/perfetto-manifest.md): the
  normative reference for the manifest.
- [How trace merging works](/docs/concepts/merging-traces.md): clock graph,
  placement rules and the machine model.
- [Multi-machine recording](/docs/learning-more/multi-machine-tracing.md):
  recording a single trace from several machines live, instead of merging
  after the fact.
