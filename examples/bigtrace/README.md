# Bigtrace local server

`tools/bigtrace_server.py` serves the Bigtrace UI API over traces on local
disk, so the Bigtrace UI can query many traces without a cloud backend. It needs
only Python 3 and the `perfetto` package in this repo.

The manifests in this directory are format examples and test data for
`python/test/bigtrace_server_*_unittest.py`.

## Quick start

```bash
tools/bigtrace_server.py /path/to/traces --generate-manifest --keep-alive
ui/run-dev-server --bigtrace    # in another shell
```

Open `http://localhost:10000/bigtrace.html?server=http://127.0.0.1:5001`.
`?server=` sets the backend endpoint, same as editing it in the connection
panel.

`/path/to/traces` is a directory or an archive. Zip and tar files (plain, gz,
bz2 or xz) are recognised by content, whatever their name, and unpacked once
to `~/.cache/perfetto/bigtrace/`; the unpacked copy, with its manifest and
query history, is reused until the archive changes.

Agents can do all of this with the [`bigtrace` skill](../../ai/skills/bigtrace/SKILL.md).

## Options

| Flag | Default | Description |
| --- | --- | --- |
| `TRACES` | `.` | Directory or archive of traces. |
| `-r`, `--recursive` | off (on for archives) | Scan subdirectories. |
| `-m`, `--manifest PATH` | `bigtrace_manifest.json` in the trace directory, if present | Manifest to load; CSV if it ends in `.csv`. |
| `-p`, `--port` | `5001` | Port to listen on. |
| `--host` | `127.0.0.1` | Address to bind. |
| `-w`, `--workers` | `min(32, CPU count)` | Traces queried in parallel. |
| `--shell-path PATH` | auto | `trace_processor_shell` to use. |
| `--keep-alive` | off | Keep traces loaded between queries. |
| `--pool-size N` | `50` | Maximum traces kept loaded with `--keep-alive`. |
| `--history-db PATH` | `.bigtrace_history.db` in the trace directory | Query history database. |
| `--origin URL` | `https://ui.perfetto.dev` | Perfetto UI that trace links open in. |
| `--cors-origin ORIGIN` | | Extra origin allowed to call the server; repeatable. |
| `--generate-manifest [OUT]` | `bigtrace_manifest.json` in the trace directory | Update the manifest before serving. |
| `-f`, `--force` | off | With `--generate-manifest`: re-extract every trace. |
| `--metadata-query SQL` | | With `--generate-manifest`: extra columns per trace. |

## Manifests

Without a manifest the trace list only has file columns (name, size, link).
`--generate-manifest` loads every trace in parallel before serving and records
its bounds, duration, process, thread and slice counts, and the build/OS rows
of the `metadata` table, so the list can be filtered and sorted by them. Later
runs pick up `bigtrace_manifest.json` from the trace directory without the
flag.

With the flag, only new or changed traces (by size and mtime) are loaded and
deleted ones are dropped, so passing it every time is cheap; `--force`
re-extracts everything. A running server re-reads its source on
`POST /refresh`.

`--metadata-query` adds the first row of a query to each trace:

```bash
tools/bigtrace_server.py /path/to/traces --generate-manifest \
  --metadata-query "SELECT count(1) AS jank_slices FROM slice WHERE name GLOB '*Choreographer*'"
tools/bigtrace_server.py /path/to/traces --generate-manifest \
  --metadata-query "$(cat metadata.sql)"
```

## Query execution

A query runs on every selected trace, `--workers` at a time. By default each
trace is loaded for the query and closed afterwards, so every query pays the
load cost. With `--keep-alive`, up to `--pool-size` traces stay loaded (least
recently used are evicted) and repeat queries only pay the query cost. A loaded
trace takes roughly 200 MB of memory; set `--pool-size` to the number of traces
you query repeatedly if memory allows.

A query stops once it has `limit` rows across all traces; traces still running
then skip their remaining rows. Cancelling or deleting a running query kills the
`trace_processor_shell` processes running it; with `--keep-alive` those traces
are loaded again by the next query.

Query history and results are stored in SQLite and survive restarts. Queries
that were running when the server stopped are marked cancelled.

## Opening traces

Clicking a trace's `link` opens it in the Perfetto UI, which fetches it from
this server:

```
https://ui.perfetto.dev/#!/?url=http://127.0.0.1:5001/trace/<uuid>/<name>&referrer=bigtrace_server
```

Like `tools/open_trace_in_ui`, `--origin` selects another UI, e.g.
`--origin http://localhost:10000`.

## Access

Browsers may only call the server from localhost origins, `https://ui.perfetto.dev`
and `--origin`; add others with `--cors-origin`. Anyone who can reach the port
can read every trace it serves, so keep the default `--host 127.0.0.1` unless you
need otherwise.

## Manifest formats

JSON (see `bigtrace_manifest.json`):

```json
{
  "trace_dir": ".",
  "schema": [
    {"name": "duration_ms", "displayName": "Duration (ms)", "type": "FLOAT64",
     "defaultVisible": true, "description": "Trace duration in milliseconds"}
  ],
  "traces": [
    {"file_path": "startup_cold.perfetto-trace", "duration_ms": 2997.996,
     "experiment_id": 101, "control_id": 102, "is_treatment": true}
  ],
  "presets": [
    {"id": "builtin:slice_count", "category": "General",
     "name": "Slice count per trace",
     "perfettoSql": "SELECT count(1) AS slice_count FROM slice;"}
  ],
  "experiments": [
    {"experimentId": 101, "experimentName": "PGO", "controlId": 102,
     "controlName": "Baseline"}
  ]
}
```

Only `traces[].file_path` is required. Paths are relative to `trace_dir`, which
is relative to the manifest. Other trace keys become metadata columns. `schema`
sets display names, types and default visibility; unlisted columns are
inferred. `experiment_id`, `control_id` and `is_treatment` drive the UI's
experiment filter.

CSV (see `bigtrace_manifest.csv`) has one row per trace with the same keys:

```csv
file_path,device_name,experiment_id,control_id,is_treatment
startup_cold.perfetto-trace,Pixel 8,101,102,true
```
