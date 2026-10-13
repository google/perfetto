---
name: bigtrace
description: >-
  Open many Perfetto traces at once in the interactive Bigtrace UI, to run
  PerfettoSQL across all of them and filter, sort and compare traces by their
  metadata. Use when the user has a directory or a zip/tar archive of traces
  and wants to explore or query them together, not one at a time. Needs a
  Perfetto source checkout.
---

# Bigtrace

`tools/bigtrace_server.py` serves a directory or archive of traces to the
Bigtrace UI, which runs a PerfettoSQL query on every trace and shows the
results as one table. Run everything from the root of a Perfetto checkout.

## Start

1. Start the server in the background and keep its output. Pass the
   directory or archive (zip, tar, tar.gz, ...) the user gave you:

   ```sh
   tools/bigtrace_server.py /path/to/traces --generate-manifest
   ```

   Archives are unpacked once to `~/.cache/perfetto/bigtrace/`.
   `--generate-manifest` loads each trace once to record metadata (duration,
   device, build, process and slice counts) for filtering and sorting; later
   runs only process new or changed traces. The server is ready when it
   prints `Bigtrace server on http://127.0.0.1:5001`. If port 5001 is taken,
   add `-p <port>`.

2. If nothing is listening on port 10000, start the UI in the background:

   ```sh
   ui/run-dev-server --bigtrace
   ```

   The first run builds the UI and takes a few minutes; it is ready when
   `curl -sf http://localhost:10000/bigtrace.html` succeeds.

3. Give the user the URL the server printed:
   `http://localhost:10000/bigtrace.html?server=http://127.0.0.1:5001`.

## Multi-Query Extraction & DuckDB Analysis

Every trace query attaches `_trace_uuid` to every row and streams its result in
C++ (`pf_scan_tp_stream`) into a DuckDB table (`result_1`, `result_2`, ... or a
custom table name).

In **PerfettoSQL** mode, click **Add Query** to extract multiple tables (e.g.,
`slices`, `sched`, `thread_state`, or `heap_objects`, `heap_refs`, `gc_roots`)
in a **single pass** over the trace corpus; double-click any sub-query tab
before running to rename its DuckDB table inline. Then switch the query engine
to **DuckDB SQL** to run cross-table analysis and joins with `trace_metadata`
using either **PerfettoSQL Next (`|>`) pipeline syntax** (`FROM ... |> ...`,
`INTERVAL INTERSECTION OF ... |> ...`, `TREE ACCUMULATE ...`, `GRAPH DOMINATOR TREE ...`)
or the `pf_interval_*`, `pf_tree_*`, `pf_graph_*` table macros. Always pass
`_trace_uuid` first in `PER (_trace_uuid, ...)` / `per := ['_trace_uuid', ...]`:

- **PerfettoSQL Next (`|>`) Pipeline Syntax (and Single-Trace Equivalence)**:
  ```sql
  INTERVAL INTERSECTION OF (
    (FROM slices |> WHERE dur >= 0) AS s,
    (FROM thread_state |> WHERE dur >= 0) AS st,
    (FROM sched |> WHERE dur >= 0) AS c
  ) PER (_trace_uuid, utid)
  |> AGGREGATE SUM(dur) / 1e6 AS dur_ms
     GROUP BY s.name, st.state, c.cpu
  |> ORDER BY dur_ms DESC
  |> LIMIT 25;
  ```
  (To run the exact same `|>` query on a single trace in PerfettoSQL, simply
  drop `_trace_uuid` from `PER (_trace_uuid, ...)`.)
- **Intervals (`pf_interval_*`)**:
  - 2-way / 3-way intersection (`slices` × `sched` × `thread_state`):
    ```sql
    SELECT s.name, st.state, c.cpu, sum(dur) / 1e6 AS dur_ms
    FROM pf_interval_intersection(
      ['(SELECT * FROM slices WHERE dur >= 0)', '(SELECT * FROM thread_state WHERE dur >= 0)', '(SELECT * FROM sched WHERE dur >= 0)'],
      ['s', 'st', 'c'],
      per := ['_trace_uuid', 'utid'],
      full_row := true
    )
    GROUP BY ALL ORDER BY dur_ms DESC LIMIT 25;
    ```
  - Flattening per track or process:
    `pf_interval_flatten_sum('(SELECT * FROM slices WHERE dur >= 0)', ['depth'], per := ['_trace_uuid', 'track_id'])`
    and `pf_interval_flatten(..., 'count(*) AS concurrency, arg_max(name, depth) AS deepest', per := ['_trace_uuid', 'track_id'])`.
- **Trees (`pf_tree_*`)**:
  - Subtree rollup: `pf_tree_accumulate_up('slices', 'dur', per := ['_trace_uuid'])`
  - Ancestor / descendant filtering:
    `pf_tree_keep_if('slices', 'descendant', '(SELECT _trace_uuid, id FROM slices WHERE name GLOB ''*doFrame*'')', per := ['_trace_uuid'])`
  - Callstack flamegraphs: `pf_tree_flatten_recursion('slices', ['name'], per := ['_trace_uuid'])` followed by `pf_tree_merge_siblings(..., ['name'], ['sum(dur) AS total_dur', 'count(*) AS call_count'], per := ['_trace_uuid'])`.
- **Graphs & Heap Dumps (`pf_graph_*` + `pf_tree_*`)**:
  - Single-pass 3-query extraction (`heap_objects`, `heap_refs` excluding `android.memory.heap_graph.excluded_refs`, and `gc_roots`), then:
    - Dominator tree + retained size:
      `pf_graph_dominator_tree('heap_objects', 'heap_refs', 'gc_roots', per := ['_trace_uuid', 'upid', 'graph_sample_ts'])` → `pf_tree_accumulate_up(..., 'self_size', per := ['_trace_uuid', 'upid', 'graph_sample_ts'])`.
    - Shortest GC root paths:
      `pf_graph_bfs_tree(...)` → `pf_tree_flatten_recursion(..., ['class_name'], ...)` → `pf_tree_key_siblings(..., ['class_name', 'heap_type'], ...)`.
- **Fleet & Experiment Aggregation**:
  - `JOIN trace_metadata USING (_trace_uuid)` and compute `quantile_cont(metric, [0.5, 0.9, 0.99])` grouped by build, device, or `is_treatment`.

See `examples/bigtrace/README.md` for the complete Cookbook of copy-pasteable
Step 1 + Step 2 recipes.

Parquet export is off by default; pass `--parquet-dir DIR` to export and reload
query tables as Parquet files, or `--duckdb-path PATH` to persist DuckDB tables
and history across restarts.

## Stop

Stop the server with Ctrl-C or SIGTERM, not SIGKILL: it shuts down its
`trace_processor_shell` children on exit, and SIGKILL leaves them running.

All options: `tools/bigtrace_server.py --help` and
`examples/bigtrace/README.md`.
