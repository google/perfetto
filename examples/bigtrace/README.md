# Bigtrace local server

`tools/bigtrace_server.py` serves the Bigtrace UI API over traces on local
disk, so the Bigtrace UI can query many traces without a cloud backend. It needs
only Python 3 and the `perfetto` package in this repo.

The manifests in this directory are format examples and test data for
`python/test/bigtrace_server_*_unittest.py`.

## Quick start

```bash
tools/bigtrace_server.py /path/to/traces --generate-manifest
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
| `-m`, `--manifest PATH` | `bigtrace_manifest.json` or `metadata.db` in the trace directory, if present | Manifest to load; CSV if it ends in `.csv`, a trace_fetch SQLite index if it ends in `.db`. |
| `-p`, `--port` | `5001` | Port to listen on. |
| `--host` | `127.0.0.1` | Address to bind. |
| `-w`, `--workers` | CPU count, at most one per 2 GB of RAM | Traces queried in parallel. |
| `--shell-path PATH` | auto | `trace_processor_shell` to use. |
| `--parquet-dir DIR` | off | Optional directory to export each query's DuckDB table as `<name>.parquet` and reload existing `.parquet` files on startup. |
| `--duckdb-path PATH` | `:memory:` | Optional file path for persistent DuckDB storage (tables and query history). |
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

## Query execution, multi-query pass, and DuckDB tables

Each trace query runs in parallel across all selected traces (`--workers` at a
time). Python acts only as the control plane: each worker hands its
`trace_processor_shell` HTTP endpoint to the C++ DuckDB extension
(`pf_scan_tp_stream` in `python/perfetto/bigtrace/duckdb_ext/src/proto_ingest.cc`),
which issues `POST /query`, decodes the chunked `QueryResult` protobuf batches
directly into columnar C++ chunks, and streams them into a DuckDB table
(`result_1`, `result_2`, ... or a custom name set via the `Table:` field in the
UI toolbar). Every result table shares a catalog-wide `_trace_uuid` `ENUM`
column (`uint16`, 2 bytes/row), and per-trace metadata columns from
`trace_metadata` are joined on demand when paging in the UI.

In **PerfettoSQL** mode, you can click **Add Query** to define multiple
PerfettoSQL queries (for example, `#1 slices` and `#2 sched`) that execute in a
**single pass** over the trace corpus—loading each trace once and populating a
separate DuckDB table for each query. **Double-click any sub-query tab** in the
sub-query bar (before it has been run) to edit its DuckDB table name inline
(just like renaming a top-level query tab); if you choose a table name that
already exists in DuckDB or collides with another slot, clicking **Run** prompts
with the standard **Overwrite Table** modal (**Cancel**, **Save as new**, or
**Overwrite**).

After running, switch the query engine to **DuckDB SQL** to run cross-table
analysis over your extracted tables using either **PerfettoSQL Next (`|>`)
pipeline syntax** (`docs/spec.md` §§2–7) or the `pf_interval_*`, `pf_tree_*`,
and `pf_graph_*` table macros. Always lead `PER (_trace_uuid, ...)` / `per :=
['_trace_uuid', ...]` with `_trace_uuid` so partitions are solved independently
per trace in parallel:

```sql
-- PerfettoSQL Next |> pipeline syntax (compiles automatically in DuckDB SQL mode):
INTERVAL INTERSECTION OF (
  (FROM slices |> WHERE dur >= 0) AS s,
  (FROM sched  |> WHERE dur >= 0) AS c
) PER (_trace_uuid, utid)
|> AGGREGATE SUM(dur) AS cpu_dur_ns, COUNT(*) AS segments
   GROUP BY _trace_uuid, s.name
|> ORDER BY cpu_dur_ns DESC;

-- Or using the pf_interval_intersection table macro directly:
SELECT *
FROM pf_interval_intersection(
  ['(SELECT * FROM "slices" WHERE dur >= 0)', '(SELECT * FROM "sched" WHERE dur >= 0)'],
  ['s', 'c'],
  per := ['_trace_uuid', 'utid'],
  full_row := true
);
```

### Copy-Pasting Between Multi-Trace DuckDB and Single-Trace PerfettoSQL

Because **DuckDB SQL** mode supports the full PerfettoSQL Next (`|>`) pipeline
syntax (`FROM ... |> WHERE ... |> EXTEND ... |> SET ... |> DROP ... |> RENAME ... |> AGGREGATE ... |> FORK AS ...`,
duration literals like `16ms`/`500us`/`1.5s`, and all `INTERVAL`, `TREE`, and
`GRAPH` operators), you can copy a `|>` pipeline directly from Bigtrace's
**DuckDB SQL** editor into a single-trace **Perfetto UI** tab (or vice versa):

- **Multi-trace -> Single-trace**: Remove `_trace_uuid` from `PER (_trace_uuid, ...)`
  (or omit `PER (_trace_uuid)` when `_trace_uuid` was the only partition key) and
  remove `_trace_uuid` from `GROUP BY` / `SELECT`.
- **Single-trace -> Multi-trace**: Prepend `_trace_uuid` to every `PER (...)`
  clause (e.g. `PER (utid)` -> `PER (_trace_uuid, utid)`, or add
  `PER (_trace_uuid)` to `TREE` / `GRAPH` stages).

Or join across any number of query tables and `trace_metadata`:

```sql
SELECT
  r1._trace_uuid,
  m.device_name,
  r1.startup_dur_ns,
  r2.binder_count
FROM result_1 r1
JOIN result_2 r2 USING (_trace_uuid)
JOIN trace_metadata m USING (_trace_uuid)
ORDER BY r1.startup_dur_ns DESC;
```

Passing `--duckdb-path PATH` persists both DuckDB tables and query execution
history to a DuckDB file across restarts. Passing `--parquet-dir DIR` exports
each query's DuckDB table to `<DIR>/<table_name>.parquet` and reloads existing
`.parquet` files into DuckDB on startup.

## Cookbook: End-to-End Multi-Trace Analysis Recipes

Every recipe below follows a two-step workflow:
1. **Step 1 — Single-Pass PerfettoSQL Extraction**: In **PerfettoSQL** mode, use
   **Add Query** to extract raw per-trace tables in a single pass over the trace
   corpus. Every output table automatically receives a `_trace_uuid` column.
2. **Step 2 — DuckDB Multi-Trace Analysis**: Switch to **DuckDB SQL** mode and
   run `pf_interval_*`, `pf_tree_*`, and `pf_graph_*` operators across all
   traces in parallel. Always pass `_trace_uuid` first in `per := ['_trace_uuid', ...]`.

---

### 1. Thread Slices × CPU Scheduling (`sched`) & Thread State (`thread_state`)

#### Step 1: Single-Pass PerfettoSQL Extraction (3 queries)

- **Query `#1` (Table: `slices`)**:
  ```sql
  SELECT
    s.id,
    s.parent_id,
    s.ts,
    s.dur,
    s.depth,
    s.name,
    s.track_id,
    tt.utid,
    t.upid
  FROM slice s
  JOIN thread_track tt ON s.track_id = tt.id
  JOIN thread t USING (utid)
  WHERE s.dur >= 0;
  ```
- **Query `#2` (Table: `sched`)**:
  ```sql
  SELECT id, ts, dur, cpu, utid
  FROM sched
  WHERE utid != 0 AND dur >= 0;
  ```
- **Query `#3` (Table: `thread_state`)**:
  ```sql
  SELECT id, ts, dur, utid, state, io_wait
  FROM thread_state
  WHERE dur >= 0;
  ```

#### Step 2a: 2-Way Intersection (`slices` × `sched`) — On-CPU vs. Wall Duration

```sql
WITH on_cpu AS (
  SELECT
    _trace_uuid,
    s.id AS slice_id,
    s.name AS slice_name,
    s.dur AS wall_dur_ns,
    sum(dur) AS cpu_dur_ns
  FROM pf_interval_intersection(
    ['(SELECT * FROM slices WHERE dur >= 0)', '(SELECT * FROM sched WHERE dur >= 0)'],
    ['s', 'c'],
    per := ['_trace_uuid', 'utid'],
    full_row := true
  )
  GROUP BY _trace_uuid, s.id, s.name, s.dur
)
SELECT
  slice_name,
  count(*) AS slice_count,
  sum(wall_dur_ns) / 1e6 AS total_wall_ms,
  sum(cpu_dur_ns) / 1e6 AS total_cpu_ms,
  round(100.0 * sum(cpu_dur_ns) / nullif(sum(wall_dur_ns), 0), 2) AS on_cpu_pct
FROM on_cpu
GROUP BY slice_name
ORDER BY total_cpu_ms DESC
LIMIT 25;
```

#### Step 2b: 3-Way Intersection (`slices` × `thread_state` × `sched`) — Slice Breakdown by State & CPU

```sql
SELECT
  s.name AS slice_name,
  st.state AS thread_state,
  c.cpu AS cpu,
  count(*) AS intersections,
  sum(dur) / 1e6 AS total_dur_ms
FROM pf_interval_intersection(
  [
    '(SELECT * FROM slices WHERE dur >= 0)',
    '(SELECT * FROM thread_state WHERE dur >= 0)',
    '(SELECT * FROM sched WHERE dur >= 0)'
  ],
  ['s', 'st', 'c'],
  per := ['_trace_uuid', 'utid'],
  full_row := true
)
GROUP BY s.name, st.state, c.cpu
ORDER BY total_dur_ms DESC
LIMIT 50;
```

#### Step 2c: Slice Self-Intersection & Interval Flattening (`pf_interval_flatten` / `pf_interval_flatten_sum`)

```sql
-- Self-intersection of slices per track (finds all overlapping/nested slice pairs):
SELECT
  _trace_uuid,
  track_id,
  a.name AS outer_Or_first_slice,
  b.name AS inner_or_second_slice,
  sum(dur) / 1e6 AS overlap_ms
FROM pf_interval_intersection(
  ['(SELECT * FROM slices WHERE dur >= 0)', '(SELECT * FROM slices WHERE dur >= 0)'],
  ['a', 'b'],
  per := ['_trace_uuid', 'track_id'],
  full_row := true
)
WHERE a.id != b.id
GROUP BY ALL
ORDER BY overlap_ms DESC
LIMIT 25;

-- Fast-path interval flattening per track (concurrency and sum of depth):
SELECT
  _trace_uuid,
  track_id,
  count(*) AS segment_count,
  max(count) AS max_concurrency,
  sum(dur) / 1e6 AS active_track_ms
FROM pf_interval_flatten_sum(
  '(SELECT * FROM slices WHERE dur >= 0)',
  ['depth'],
  per := ['_trace_uuid', 'track_id']
)
GROUP BY _trace_uuid, track_id
ORDER BY active_track_ms DESC;

-- General interval flattening per process (upid) with custom DuckDB aggregates:
SELECT
  _trace_uuid,
  upid,
  ts,
  dur,
  concurrency,
  deepest_slice
FROM pf_interval_flatten(
  '(SELECT * FROM slices WHERE dur >= 0 AND upid IS NOT NULL)',
  'count(*) AS concurrency, arg_max(name, depth) AS deepest_slice',
  per := ['_trace_uuid', 'upid']
)
ORDER BY dur DESC
LIMIT 50;
```

---

### 2. Slice Tree Hierarchies (`pf_tree_*`)

#### Step 1: PerfettoSQL Extraction (Table: `slices`)

```sql
SELECT
  s.id,
  s.parent_id,
  s.ts,
  s.dur,
  s.depth,
  s.name,
  s.track_id,
  tt.utid
FROM slice s
JOIN thread_track tt ON s.track_id = tt.id
WHERE s.dur >= 0;
```

#### Step 2a: Inclusive Subtree Time (`pf_tree_accumulate_up`) & Exclusive Self-Time (`dur - children_dur`)

```sql
-- Inclusive subtree sum (note: for strict nested slice intervals, self + descendants
-- sums each node's dur up the tree; for exclusive self-time, subtract direct children's dur):
WITH child_sums AS (
  SELECT
    _trace_uuid,
    parent_id AS id,
    sum(dur) AS children_dur
  FROM slices
  WHERE parent_id IS NOT NULL
  GROUP BY _trace_uuid, parent_id
),
acc AS (
  SELECT _trace_uuid, id, total AS subtree_dur_sum
  FROM pf_tree_accumulate_up('slices', 'dur', per := ['_trace_uuid'])
)
SELECT
  s.name,
  count(*) AS invocations,
  sum(s.dur) / 1e6 AS inclusive_wall_ms,
  sum(s.dur - coalesce(c.children_dur, 0)) / 1e6 AS exclusive_self_ms,
  sum(a.subtree_dur_sum) / 1e6 AS accumulated_subtree_ms
FROM slices s
JOIN acc a USING (_trace_uuid, id)
LEFT JOIN child_sums c USING (_trace_uuid, id)
GROUP BY s.name
ORDER BY exclusive_self_ms DESC
LIMIT 25;
```

#### Step 2b: Filtering Descendants & Ancestors (`pf_tree_keep_if`)

```sql
-- All slices strictly descended from 'Choreographer#doFrame':
SELECT
  name,
  count(*) AS count_under_do_frame,
  sum(dur) / 1e6 AS total_dur_ms
FROM pf_tree_keep_if(
  'slices',
  'descendant',
  '(SELECT _trace_uuid, id FROM slices WHERE name GLOB ''*Choreographer#doFrame*'')',
  per := ['_trace_uuid']
)
GROUP BY name
ORDER BY total_dur_ms DESC
LIMIT 25;

-- All ancestor slices enclosing a 'binder transaction':
SELECT
  name,
  count(*) AS count_enclosing_binder,
  sum(dur) / 1e6 AS total_dur_ms
FROM pf_tree_keep_if(
  'slices',
  'ancestor',
  '(SELECT _trace_uuid, id FROM slices WHERE name = ''binder transaction'')',
  per := ['_trace_uuid']
)
GROUP BY name
ORDER BY count_enclosing_binder DESC
LIMIT 25;
```

#### Step 2c: Collapsing Recursive Frames & Building Aggregated Callstack Flamegraphs

```sql
-- 1. Collapse direct recursion (A -> A -> A becomes siblings under A's parent),
-- 2. Unify identical root-to-node callstack paths per trace with pf_tree_merge_siblings:
CREATE OR REPLACE TEMP VIEW slices_norec AS
SELECT *
FROM pf_tree_flatten_recursion('slices', ['name'], per := ['_trace_uuid']);

SELECT
  _trace_uuid,
  id,
  parent_id,
  name,
  total_dur / 1e6 AS total_dur_ms,
  call_count
FROM pf_tree_merge_siblings(
  'slices_norec',
  ['name'],
  ['sum(dur) AS total_dur', 'count(*) AS call_count'],
  per := ['_trace_uuid']
)
ORDER BY total_dur DESC
LIMIT 50;
```

---

### 3. Java Heap Dump Analysis (`pf_graph_*` + `pf_tree_*`)

#### Step 1: Single-Pass 3-Query PerfettoSQL Extraction

To match Trace Processor's `android.memory.heap_graph.dominator_tree` exactly,
exclude weak, soft, finalizer, and phantom reference edges via
`android.memory.heap_graph.excluded_refs`:

- **Query `#1` (Table: `heap_objects`)**:
  ```sql
  SELECT
    o.id,
    o.upid,
    o.graph_sample_ts,
    o.self_size,
    o.native_size,
    o.heap_type,
     coalesce(c.deobfuscated_name, c.name) AS class_name
  FROM heap_graph_object o
  JOIN heap_graph_class c ON o.type_id = c.id
  WHERE o.reachable != 0;
  ```
- **Query `#2` (Table: `heap_refs`)**:
  ```sql
  INCLUDE PERFETTO MODULE android.memory.heap_graph.excluded_refs;

  SELECT
    o.upid,
    o.graph_sample_ts,
    r.owner_id AS src,
    r.owned_id AS dst
  FROM heap_graph_reference r
  JOIN heap_graph_object o ON r.owner_id = o.id
  WHERE o.reachable != 0
    AND r.owned_id IS NOT NULL
    AND r.id NOT IN _excluded_refs;
  ```
- **Query `#3` (Table: `gc_roots`)**:
  ```sql
  SELECT
    id,
    upid,
    graph_sample_ts
  FROM heap_graph_object
  WHERE root_type IS NOT NULL;
  ```

#### Step 2a: Exact Retained Heap Size (`pf_graph_dominator_tree` → `pf_tree_accumulate_up`)

```sql
CREATE OR REPLACE TEMP VIEW dom_tree AS
SELECT *
FROM pf_graph_dominator_tree(
  'heap_objects',
  'heap_refs',
  'gc_roots',
  per := ['_trace_uuid', 'upid', 'graph_sample_ts'],
  full_row := true
);

WITH retained AS (
  SELECT
    _trace_uuid,
    upid,
    graph_sample_ts,
    id,
    total AS dominated_size_bytes
  FROM pf_tree_accumulate_up(
    'dom_tree',
    'self_size',
    per := ['_trace_uuid', 'upid', 'graph_sample_ts']
  )
)
SELECT
  d.class_name,
  count(DISTINCT d._trace_uuid) AS traces_present,
  count(*) AS object_count,
  sum(d.self_size) AS total_shallow_bytes,
  max(r.dominated_size_bytes) AS max_object_retained_bytes,
  sum(r.dominated_size_bytes) FILTER (WHERE d.parent_id IS NULL) AS root_retained_bytes
FROM dom_tree d
JOIN retained r USING (_trace_uuid, upid, graph_sample_ts, id)
GROUP BY d.class_name
ORDER BY max_object_retained_bytes DESC
LIMIT 25;
```

#### Step 2b: Shortest GC Root Retention Paths (`pf_graph_bfs_tree` → `pf_tree_flatten_recursion` → `pf_tree_key_siblings`)

```sql
CREATE OR REPLACE TEMP VIEW heap_bfs AS
SELECT *
FROM pf_graph_bfs_tree(
  'heap_objects',
  'heap_refs',
  'gc_roots',
  per := ['_trace_uuid', 'upid', 'graph_sample_ts'],
  full_row := true
);

CREATE OR REPLACE TEMP VIEW heap_bfs_norec AS
SELECT *
FROM pf_tree_flatten_recursion(
  'heap_bfs',
  ['class_name'],
  per := ['_trace_uuid', 'upid', 'graph_sample_ts']
);

SELECT
  _trace_uuid,
  node_id,
  parent_node_id,
  class_name,
  heap_type,
  count(*) AS instance_count,
  sum(self_size) AS shallow_size_bytes
FROM pf_tree_key_siblings(
  'heap_bfs_norec',
  ['class_name', 'heap_type'],
  key_col := 'node_id',
  parent_key_col := 'parent_node_id',
  per := ['_trace_uuid', 'upid', 'graph_sample_ts']
)
GROUP BY _trace_uuid, node_id, parent_node_id, class_name, heap_type
ORDER BY shallow_size_bytes DESC
LIMIT 50;
```

---

### 4. Cross-Table Fleet & Experiment Aggregation with `trace_metadata`

Join any DuckDB operator output with `trace_metadata` (`USING (_trace_uuid)`)
to compute fleet-wide P50 / P90 / P99 percentiles across devices, builds, or
A/B experiment arms:

```sql
WITH per_trace_cpu AS (
  SELECT
    _trace_uuid,
    s.name AS slice_name,
    sum(dur) / 1e6 AS on_cpu_ms
  FROM pf_interval_intersection(
    ['(SELECT * FROM slices WHERE dur >= 0)', '(SELECT * FROM sched WHERE dur >= 0)'],
    ['s', 'c'],
    per := ['_trace_uuid', 'utid'],
    full_row := true
  )
  WHERE s.name GLOB '*Choreographer#doFrame*'
  GROUP BY _trace_uuid, s.name
)
SELECT
  coalesce(m.android_build_fingerprint, 'unknown') AS build,
  m.is_treatment,
  count(DISTINCT p._trace_uuid) AS trace_count,
  round(quantile_cont(p.on_cpu_ms, 0.50), 2) AS p50_on_cpu_ms,
  round(quantile_cont(p.on_cpu_ms, 0.90), 2) AS p90_on_cpu_ms,
  round(quantile_cont(p.on_cpu_ms, 0.99), 2) AS p99_on_cpu_ms
FROM per_trace_cpu p
JOIN trace_metadata m USING (_trace_uuid)
GROUP BY ALL
ORDER BY build, m.is_treatment;
```

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
