---
name: perfetto-infra-querying-traces
description: Use when the user wants to load a Perfetto trace, run a
  PerfettoSQL query against it, or discover which tables, views, columns,
  or stdlib modules are available. Covers trace_processor invocation, the
  long-running RPC mode, and how to compose stdlib modules into a query.
---

# Querying Perfetto traces

This skill teaches you how to extract data from a Perfetto trace file
(`.pftrace`, `.perfetto-trace`, `.pb`) using `trace_processor` and
PerfettoSQL.

The `trace_processor` binary is what every other Perfetto analysis tool
runs on top of, including the Perfetto UI. Reference docs:
<https://perfetto.dev/docs/analysis/trace-processor>.

> **Prerequisite — getting `trace_processor`.** This skill assumes
> `trace_processor` is already on `PATH` (and, for the long-running RPC
> mode below, that the Python client is installed). If neither is set
> up, follow whatever skill the user's environment provides for
> acquisition — typically `perfetto-infra-getting-trace-processor` for the
> open-source path, or a team-specific variant inside Google or other
> restricted environments. The rest of this skill is intentionally
> orthogonal to *how* you got `trace_processor`.

## Quickstart

To run a single query and exit:

```sh
trace_processor query TRACE_FILE "SELECT ts, dur FROM slice LIMIT 10"
```

Multiple statements separated by `;` are supported in one invocation.

## Long-running mode (preferred for iteration)

Reparsing a trace on every query is slow — for a multi-GB trace it's tens
of seconds, every time. When you expect to run more than a couple of
queries, start the shell once as an HTTP RPC server and drive it from
the Python client. (If the Python client is not installed yet, see the
`getting-trace-processor` skill or the team-specific equivalent.)

```sh
# Terminal A: pick a random high port and start the server on it.
# Always pass --port explicitly: the default (9001) is also used by
# the Perfetto UI, and a fixed port collides with any other agent
# already running a trace_processor server.
PORT=$((9100 + RANDOM % 900))
trace_processor server http --port $PORT TRACE_FILE
```

```python
# Terminal B (or any Python process): connect to the running server.
# PORT is the value chosen in Terminal A.
from perfetto.trace_processor import TraceProcessor

tp = TraceProcessor(addr='127.0.0.1:PORT')
for row in tp.query('SELECT ts, dur, name FROM slice WHERE dur > 5e8 LIMIT 5'):
    print(row.dur, row.name)

# Pandas is also supported if the dependency is installed:
df = tp.query('SELECT ts, dur, name FROM slice LIMIT 100').as_pandas_dataframe()

tp.close()
```

The server keeps the trace parsed in memory; each `tp.query()` call is
just a query against the existing session. This is the same RPC channel
`ui.perfetto.dev` uses when you load a trace there.

Notes:

- Use `'127.0.0.1:PORT'`, not `'localhost:PORT'`. The server binds on
  IPv4/IPv6 explicitly and `localhost` resolution can pick an interface
  that isn't bound on macOS.
- A quick liveness check from the shell:
  `curl http://127.0.0.1:PORT/status` returns plain JSON-ish status
  (loaded path, version) and is the fastest way to confirm the server
  is up.
- The on-the-wire `/query`, `/parse`, `/rpc` endpoints take protobuf-
  encoded `QueryArgs`/`TraceProcessorRpc` payloads. **Do not hand-craft
  HTTP calls with `curl`** — use the Python client (or the WASM
  client embedded in the UI). Hand-crafted calls work for `/status` only.
- If you want to stay in the C++ shell instead of Python, a one-shot
  `trace_processor query TRACE_FILE "..."` is fine for a handful of
  queries; the server is the right answer the moment iteration starts.

## Discovering what's in the trace

PerfettoSQL ships with **intrinsic table-functions** for browsing the
loaded standard library — modules, tables/views, functions, macros. Use
these to find what's available; use a plain `LIMIT 0` query to read the
column schema of any specific table, view, or query result.

> **Intrinsic surface — not stable API.** The `__intrinsic_*` names below
> are an implementation detail of trace processor. They're fair game for
> an agent to use during a session because this skill is loaded, but
> **don't bake `__intrinsic_*` names into committed scripts, dashboards,
> or stdlib modules** — they can change without notice.

```sql
-- 1. List every stdlib module currently available.
SELECT package, module FROM __intrinsic_stdlib_modules ORDER BY 1, 2;

-- 2. List the tables/views a specific module exposes
--    (after INCLUDE PERFETTO MODULE).
INCLUDE PERFETTO MODULE slices.with_context;
SELECT name, type, exposed, description
FROM __intrinsic_stdlib_tables('slices.with_context');

-- 3. List functions / macros a module exposes.
SELECT name, return_type, args
FROM __intrinsic_stdlib_functions('slices.with_context');
SELECT name, return_type, args
FROM __intrinsic_stdlib_macros('android.memory.heap_graph.helpers');

-- 4. Read the column schema of any table, view, or query.
--    LIMIT 0 returns the result header with no row scan; trace_processor
--    prints "column N = <name>" lines for each column.
SELECT * FROM slice LIMIT 0;
SELECT * FROM thread_or_process_slice LIMIT 0;
SELECT * FROM (SELECT ts, dur, name FROM slice WHERE dur > 0) LIMIT 0;
```

Useful starting points for any trace:

| View           | What's in it                                                    |
| -------------- | --------------------------------------------------------------- |
| `slice`        | Atrace slices, async slices, anything with a duration on a track |
| `thread`       | One row per thread                                              |
| `process`      | One row per process                                             |
| `thread_state` | Scheduling state transitions (Running, Runnable, Sleeping, …)   |
| `sched_slice`  | When threads were on-CPU                                        |
| `counter`      | Time-series counter samples                                     |
| `track`        | Every track in the trace; join on `track_id` to most other tables |

Static reference for the public surface (does not require a running
trace_processor): <https://perfetto.dev/docs/analysis/sql-tables>.

## Using the standard library

Most useful queries are *much* shorter when you build on stdlib modules
instead of joining raw tables yourself. Generated stdlib reference:
<https://perfetto.dev/docs/analysis/stdlib-docs>.

Include a module before referencing the views, tables or macros it
defines:

```sql
INCLUDE PERFETTO MODULE slices.with_context;

SELECT name, dur, thread_name, process_name
FROM thread_or_process_slice
WHERE dur > 1e9                     -- slices longer than 1s
ORDER BY dur DESC
LIMIT 20;
```

A few commonly used modules to know:

- `slices.with_context` — slice rows joined with their thread / process.
- `sched.with_context` — `sched_slice` joined with thread / process.
- `android.startup.startups` — one row per app startup.
- `stacks.cpu_profiling` — flat samples and call-graph helpers.
- `android.memory.heap_graph.dominator_tree` — retained-size analysis for
  Java heap dumps (see the `perfetto-workflow-android-heap-dump`
  skill for usage).

The module name maps directly to the file path under the stdlib root:
`foo.bar` lives at `foo/bar.sql`. Browse the full list at the stdlib
reference linked above.

## Tips for writing good PerfettoSQL

- **Reach for stdlib first.** If you find yourself joining `slice` to
  `thread_track` to `thread` to `process`, there is almost certainly a
  stdlib module that already does it. Check the stdlib reference before
  writing the join.
- **Filter on `dur > 0` carefully.** Some slices have `dur = -1` (still
  open at trace end) and some have `dur = 0` (instant events). Be explicit
  about which you mean.
- **Avoid `SELECT *` in saved queries.** Trace processor table schemas can
  gain columns; pin the columns you actually use.
- **Don't expose raw IDs in summaries.** Columns like `id`, `utid`, `upid`,
  `track_id` are not stable across traces or even runs of trace_processor
  on the same trace. They are fine to use *inside* a query as join keys,
  but join out to a stable name (`thread.name`, `process.name`,
  `slice.name`) before reporting results to the user.
- **Use `EXPLAIN QUERY PLAN` if a query is slow.** It shows whether SQLite
  is using indexes. Counter and slice tables have built-in indexes on `ts`
  and `track_id`; queries that don't filter on either will scan the whole
  table.
- **Materialise expensive intermediate results.** `CREATE PERFETTO TABLE
  foo AS SELECT ...` caches the result so subsequent queries don't redo
  the work. Use `CREATE PERFETTO VIEW` if you want lazy evaluation.

## Where to look for more

- Language tour:
  <https://perfetto.dev/docs/analysis/perfetto-sql-getting-started>
- Trace processor reference:
  <https://perfetto.dev/docs/analysis/trace-processor>
- Generated table reference:
  <https://perfetto.dev/docs/analysis/sql-tables>
- Generated stdlib reference:
  <https://perfetto.dev/docs/analysis/stdlib-docs>
