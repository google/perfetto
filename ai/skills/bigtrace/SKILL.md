---
name: bigtrace
description: >-
  Run PerfettoSQL on many Perfetto traces at once and analyse the combined
  results with DuckDB SQL, from the command line (or the Bigtrace UI for
  humans). Use when the user has a directory or zip/tar archive of traces and
  wants to query, aggregate or compare them together rather than one at a
  time. Needs a Perfetto source checkout.
---

# Bigtrace

`tools/bigtrace_server.py` loads every trace in its own
`trace_processor_shell`, runs a PerfettoSQL query on all of them in parallel
and collects the rows in a DuckDB table with a `_trace_uuid` column. Follow-up
analysis is DuckDB SQL over those tables and `trace_metadata` (one row per
trace), which answers in milliseconds without reloading traces. Run
everything from the root of a Perfetto checkout. Details:
`tools/bigtrace_server.py <serve|query|sql> --help` and
`docs/analysis/bigtrace-local.md`.

## Workflow

1. Requirements: `pip install duckdb pyarrow`, plus `g++` and `curl` for the
   extension the server builds on first use. Point `$SHELL_PATH` at a
   `trace_processor_shell` (e.g. `out/linux/trace_processor_shell`), or one is
   downloaded.
2. Start the server in the background and wait for its "Serving N traces"
   line (default port 5001; another port with `-p`, then pass `--server
   http://127.0.0.1:PORT` to the commands below):

   ```bash
   tools/bigtrace_server.py serve /path/to/traces -r --generate-manifest &
   ```

   `--generate-manifest` adds per-trace metadata (duration, counts, build) to
   `trace_metadata`; skip it to start faster.
3. Extract tables with PerfettoSQL. Several queries in one call share one
   pass over the traces; name each table with `--table`:

   ```bash
   tools/bigtrace_server.py query --table slices --table threads \
     "SELECT name, dur, track_id FROM slice" "SELECT utid, name FROM thread"
   ```

   Pick traces with `--filter "COLUMN OP VALUE"` over `trace_metadata`.
   stdout has the first rows as CSV; stderr has the table names, row counts
   and per-trace errors. Exit status: 0 ok, 2 some traces failed, 1 failure.
   Prefer per-trace aggregation in PerfettoSQL when the raw rows are not
   needed.
4. Analyse with DuckDB SQL; output is CSV, capped by `--limit` (default 100),
   so aggregate rather than dump:

   ```bash
   tools/bigtrace_server.py sql "SELECT m.file_name, count(*) AS n, sum(dur) AS total
     FROM slices JOIN trace_metadata m USING (_trace_uuid) GROUP BY ALL ORDER BY total DESC"
   tools/bigtrace_server.py sql "FROM duckdb_tables()"          # tables
   tools/bigtrace_server.py sql "SELECT execution->>'tableName', execution->>'status' FROM _bigtrace_history"
   ```

   `CREATE TABLE x AS ...` keeps intermediate results; `latest` is the last
   result table. DuckDB SQL also accepts PerfettoSQL's `|>` chain syntax
   (`WHERE`, `SELECT`, `EXTEND`, `SET`, `DROP`, `RENAME`,
   `AGGREGATE ... GROUP BY`, `JOIN`, `ORDER BY`, `LIMIT`, `AS`, `FORK AS`) and
   duration literals (`16ms`, `1.5s`):

   ```bash
   tools/bigtrace_server.py sql "
     FROM slices AS s
     |> WHERE s.dur > 16ms
     |> AGGREGATE count(*) AS janky GROUP BY s._trace_uuid
     |> JOIN trace_metadata AS m USING (_trace_uuid)
     |> SELECT m.file_name, janky |> ORDER BY janky DESC"
   ```

   DuckDB SQL also supports interval, tree and graph operators partitioned by
   `_trace_uuid` (see `python/perfetto/bigtrace/duckdb_ext/docs/*.md`):
   - `INTERVAL INTERSECTION OF (slices AS s, sched AS c) PER _trace_uuid, utid`
   - `INTERVAL FLATTEN`, `MERGE`, `SPLIT`, `SUBTRACT`, `FILL GAPS`, `JOIN`
   - `TREE ACCUMULATE UP|DOWN SUM(dur) AS total PER _trace_uuid`, `KEEP IF`,
     `PRUNE`, `CONTRACT`, `FLATTEN RECURSION`
   - `GRAPH DOMINATOR TREE NODES obj EDGES refs FROM roots PER _trace_uuid`,
     `GRAPH BFS`, `GRAPH DFS`

## UI, for humans

Open `https://ui.perfetto.dev/bigtrace.html?server=http://127.0.0.1:5001`
(or serve the UI locally with `ui/run-dev-server --bigtrace`, then
`http://localhost:10000/bigtrace.html?server=...`). The trace list can be
filtered by metadata; query tabs run PerfettoSQL (with **Add Query** for
several tables in one pass) or DuckDB SQL over the result tables.
