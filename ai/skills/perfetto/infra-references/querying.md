# Querying Perfetto traces

This reference explains how to extract data from a Perfetto trace file
(`.pftrace`, `.perfetto-trace`, `.pb`) using `trace_processor` and
PerfettoSQL. Read it for ad-hoc querying outside a guided workflow; the
workflows under `$SKILL_ROOT/workflows/` carry their own queries.

The `trace_processor` binary is what every other Perfetto analysis tool
runs on top of, including the Perfetto UI. Reference docs:
<https://perfetto.dev/docs/analysis/trace-processor>.

> **Prerequisite — `trace_processor` must be invokable.** Before
> running any of the shell commands below, read
> `$SKILL_ROOT/environment-references/setup.md`. It defines how to make
> the bare `trace_processor` commands below work in this environment.

## Querying a trace: sessions

Querying goes through a **session**: load the trace once into a named
background session, then run every query against it with `--remote`.
Parsing a trace is the expensive part (tens of seconds for a multi-GB
trace); the session pays it once, and every real analysis runs more than
one query.

```sh
# 1. Load the trace into a background session — once per trace.
#    Pick a descriptive session name (e.g. derived from the trace file).
trace_processor server unix --name mysession --daemonize TRACE_FILE

# 2. Run queries against the warm session: instant, no reparse.
trace_processor query --remote mysession \
  "SELECT ts, dur, name FROM slice WHERE dur > 5e8 LIMIT 5"

# 3. When you are completely done with the trace:
trace_processor server kill mysession
```

Multiple statements separated by `;` are supported in one invocation.

Session rules:

- Session names are managed by trace_processor in a per-user session
  directory — there are no ports to choose and no collisions with other
  agents or the Perfetto UI.
- **Session state persists across `query --remote` calls.** A
  `CREATE PERFETTO TABLE` or `INCLUDE PERFETTO MODULE` run in one call is
  visible in the next, so materializing intermediate results pays off
  across invocations.
- Flags that configure trace loading (`--full-sort`,
  `--add-sql-package`, ...) belong on the `server unix` invocation, not
  on `query --remote` — the client rejects them with an explanatory
  error.
- `--remote` also accepts an absolute `*.sock` path or `host:port`;
  names are the common case.
- Forgotten sessions are reaped automatically after 30 minutes idle
  (`--idle-timeout`), but kill your session when the analysis is done.

`TRACE_FILE` can be a local path, an `http(s)://` URL, or a Perfetto UI
share link (`https://ui.perfetto.dev/#!/?s=<hash>`) — in the last two
cases trace_processor downloads the trace for you (cached under
`~/.cache/perfetto/` or the platform equivalent), resolving the share
link to its underlying trace first.

For a single throwaway query on a small trace you *can* skip the session
(`trace_processor query TRACE_FILE "..."` parses, queries, and exits),
but treat that as the exception: it re-parses the trace on every
invocation and forgets created tables and included modules between
calls. Default to a session.

## Discovering what's in the trace

PerfettoSQL provides a searchable intrinsic table containing every loaded
standard-library module, table, view, function, table function, and macro. Use
it to verify whether the Standard Library already provides the needed
abstraction before drafting custom logic.

**Mandatory Schema Check:** Do not guess column names or join keys. Always
use a plain `LIMIT 0` query to read the exact column schema of any specific
table, view, or query result before drafting your query.

> **Intrinsic surface — not stable API.** The `__intrinsic_*` names below
> are an implementation detail of trace processor. They're fair game for
> an agent to use during a session because this reference is loaded, but
> **don't bake `__intrinsic_*` names into committed scripts, dashboards,
> or stdlib modules** — they can change without notice.

```sql
-- Search every documented stdlib object. Start with one precise phrase or
-- API-like term. In a regexp, `|` means alternatives: prefer
-- `breadth.first|bfs`, not a broad expression such as `breadth|graph|search`.
WITH search(pattern) AS (VALUES ('precise phrase|api_term'))
SELECT o.qualified_name, o.object_type, o.short_description
FROM __intrinsic_stdlib_objects AS o, search AS s
WHERE o.exposed = 1
  AND regexp(s.pattern, o.summary, 'i')
ORDER BY regexp(s.pattern, o.qualified_name, 'i') DESC,
         o.qualified_name
LIMIT 10;

-- Once a candidate is found, read one human-friendly document containing its
-- identity, description, arguments, return value, and result columns.
SELECT o.summary
FROM __intrinsic_stdlib_objects AS o
WHERE o.qualified_name =
  'intervals.overlap.intervals_overlap_count_by_group';

-- Read the runtime schema of any table, view, or query. LIMIT 0 returns the
-- result header with no row scan.
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
  Java heap dumps (see `$SKILL_ROOT/workflows/android_memory/heap_dump.md` for usage).

The module name maps directly to the file path under the stdlib root:
`foo.bar` lives at `foo/bar.sql`. Browse the full list at the stdlib
reference linked above.


## Tips for writing good PerfettoSQL

- **Reach for stdlib first.** If you find yourself joining `slice` to
  `thread_track` to `thread` to `process`, there is almost certainly a
  stdlib module that already does it. Check the stdlib reference before
  writing the join.
- **Filter on `dur > 0` and Trace Boundaries carefully.** Some slices have
  `dur = -1` (still open at trace end) and some have `dur = 0` (instant
  events). Be explicit about which you mean. When calculating a bounding box
  (for example, `ts + dur`) or summing durations (`SUM(dur)`), handle
  incomplete durations using: `IIF(dur = -1, trace_end() - ts, dur)`.
- **Robust State Transitions.** Avoid manual timestamp arithmetic (for
  example, `ts + dur = next.ts`) to join adjacent events. Rely on standard
  library modules (for example, `sched.runnable`, `linux.perf.counters`,
  `intervals.overlap`) which safely handle trace gaps and preemptions.
- **Working with Identifiers:**
  - **Use Unique Identifiers for Joins:** When writing SQL queries in
    Perfetto, you must join tables using `utid` (unique thread ID) or `upid`
    (unique process ID) instead of the regular `tid` or `pid`. **Why it's
    useful**: The operating system recycles `TIDs` and `PIDs`, while `UTIDs`
    and `UPIDs` remain unique for the lifetime of the trace, which prevents
    incorrect joins.
  - Columns like `id`, `utid`, `upid`, `track_id` are not stable across traces
    or even runs of trace_processor on the same trace. You can use them
    **inside** a query as join keys, but alongside the IDS, always join out to
    a stable name (`thread.name`, `process.name`, `slice.name`) when reporting
    results to the user.
  - **Materialise expensive intermediate results.** `CREATE PERFETTO TABLE foo
    AS SELECT ...` caches the result so subsequent queries don't redo the work.
    - *Note for `SPAN_JOIN`:* Intermediate tables fed into a `SPAN_JOIN` must
      be materialized using `CREATE PERFETTO TABLE`, not `CREATE VIEW`.
- **Idempotency.** Ensure queries are idempotent to prevent "already exists"
  errors during multiple executions.
  - For Perfetto objects, always use `CREATE OR REPLACE`: `CREATE OR REPLACE
    PERFETTO {TABLE|VIEW|MACRO|FUNCTION}`.
  - For SQLite Virtual Tables (such as `SPAN_JOIN`), `CREATE OR REPLACE` is
    not supported. Explicitly drop them first: `DROP TABLE IF EXISTS
    my_table; CREATE VIRTUAL TABLE my_table USING SPAN_JOIN(...);`
  - For standard SQLite indexes, prepend `DROP INDEX IF EXISTS index_name;`.
- **`SPAN_JOIN` safety.** `SPAN_JOIN` will crash if intervals **within the
  same input table** overlap. Always use the `PARTITIONED {column}` (for
  example, `PARTITIONED track_id`) clause to isolate intervals.
- **Avoid `SELECT *` in saved queries.** Trace processor table schemas can
  gain columns; pin the columns you actually use.
- **Use `EXPLAIN QUERY PLAN` if a query is slow.** It shows whether SQLite is
  using indexes. Counter and slice tables have built-in indexes on `ts` and
  `track_id`; queries that don't filter on either will scan the whole table.
- **Argument Extraction:** Use `EXTRACT_ARG(arg_set_id, 'key')` to fetch event
  properties instead of manually joining the `args` table.
- **JSON Parsing:** When dealing with JSON text, use standard SQLite JSON
  functions (for example, `json_extract()`) to extract values.
- **String Matching (Avoid `LIKE`).** Use `GLOB` or `regexp()` instead of
  `LIKE`. `LIKE`
  causes performance bottlenecks and treats underscores (`_`) as wildcards,
  leading to bugs.
  - **Exact matches:** Use `=`.
  - **Substring matches:** Use `GLOB` with `*` (for example, `name GLOB
    '*RenderThread*'`).
  - **Case-insensitive matches:** Use `regexp(pattern, input, 'i')`. It
    performs a case-insensitive partial match without `LOWER()` or `*`
    wildcards (for example, `regexp('renderthread', name, 'i')`).
- **Alias Precision.** Always prefix column names with table or view alias,
  that is: `{alias}.{column_name}`.

- **Avoid Custom SQL Functions (Performance):** Avoid creating custom
  functions via `CREATE PERFETTO FUNCTION` for large datasets or per-row
  computations, as scalar/table function calls in SQLite/Perfetto incur
  substantial per-row execution overhead. Instead, prefer direct SQL queries,
  inline expressions, CTEs, or precomputed stdlib views/tables.

## Common Analysis Patterns

- **Time Overlaps & Intersections (SPAN_JOIN):**
  - **Concept:** `SPAN_JOIN` is a custom operator table that computes the time-interval
    intersection of spans from two tables or views. A "span" is any row with `ts`
    (timestamp) and `dur` (duration) columns. The output virtual table contains the
    intersected `ts` and `dur` representing the exact overlapping time window, along with
    all columns from both input tables.
  - **Partitioning:** An optional partition column can be specified on neither, either,
    or both tables (`PARTITIONED col`). If specified on both, the column name must match.
    Partition columns **must be integers** (e.g. `cpu`, `upid`, `utid`, `track_id`).
    String columns are *not* supported directly; convert to integers with `HASH(str_col)`
    if necessary.
  - **No Internal Overlaps:** Spans within the same input table and partition **must not
    overlap**. `SPAN_JOIN` assumes non-overlapping slices per partition; if internal
    intervals overlap, incorrect rows will silently be produced. Ensure input tables are
    cleanly partitioned and materialized via `CREATE PERFETTO TABLE`.
  - **Syntax & Idempotency:** Because `SPAN_JOIN` creates an SQLite virtual table,
    `CREATE OR REPLACE` is not supported. Always drop first:
    ```sql
    DROP TABLE IF EXISTS sched_with_frequency;
    CREATE VIRTUAL TABLE sched_with_frequency
    USING SPAN_JOIN(sp_sched PARTITIONED cpu, sp_frequency PARTITIONED cpu);
    ```
  - **Variants:** `SPAN_LEFT_JOIN` (left outer interval intersection) and `SPAN_OUTER_JOIN`
    (full outer interval intersection) are also available.
  - **Fallback (When `SPAN_JOIN` is not applicable):** If computing custom overlap durations
    between two interval sets `[start1, end1]` and `[start2, end2]` manually:
    - **Condition:** Overlap if `start1 < end2 AND start2 < end1`.
    - **Duration:** `MIN(end1, end2) - MAX(start1, start2)`.
    - **Incomplete Slices:** Handle `dur = -1` via `ts + IIF(dur = -1, trace_end() - ts, dur)`.
- **Window Size:** When looking for events around a specific timestamp, start
  with 100ms as the window size.
- **Total Duration:** To calculate the total time spent in slices matching a
  specific name pattern (for example, `*{name_pattern}*`), sum their durations:
  ```sql
  SELECT
    count(*) as total_count,
    sum(IIF(slice.dur = -1, trace_end() - slice.ts, slice.dur)) / 1000000.0 as total_dur_ms
  FROM slice
  WHERE slice.name GLOB '*{name_pattern}*';
  ```
- **CPU Cluster & Frequency Analysis:**
  - Classify CPU core clusters (big, mid, little) with `INCLUDE PERFETTO MODULE android.cpu.cluster_type;` (`android_cpu_cluster_mapping`) and calculate time-weighted frequency via `INCLUDE PERFETTO MODULE linux.cpu.frequency;`.
- **Binder Transaction Analysis:**
  - Query cross-process Binder calls using stdlib views like `android_binder_txns` or `android_binder_metrics_by_process` (`INCLUDE PERFETTO MODULE android.binder;`), grouping by process names (`client_process`, `server_process`, `process_name`).
- **Low Memory Killer (LMK) & Process Memory Analysis:**
  - Identify killed processes and timestamps with `INCLUDE PERFETTO MODULE android.memory.lmk;` (`android_lmk_events`).
  - Query process RSS and swap changes over time with `INCLUDE PERFETTO MODULE linux.memory.process;` (`memory_rss_and_swap_per_process`).
- **Wakeup Chain & Thread Scheduling Analysis:**
  - To trace which threads woke each other up leading into a slice execution, locate the slice via `slices.with_context` (`thread_slice`), correlate with `thread_state` where `state = 'R'` (Runnable state preceding execution), and recursively traverse `waker_utid`, joining out to `thread.name`:
    ```sql
    INCLUDE PERFETTO MODULE slices.with_context;

    WITH RECURSIVE wakeup_chain(depth, utid, waker_utid, ts) AS (
      -- Base: find the runnable state preceding the target slice
      SELECT 0 AS depth, s.utid, ts.waker_utid, ts.ts
      FROM thread_slice s
      JOIN thread_state ts ON s.utid = ts.utid AND ts.state = 'R'
        AND ts.ts + ts.dur = s.ts
      WHERE s.is_main_thread = 1 AND s.name GLOB '*Choreographer*'
      UNION ALL
      -- Recurse: find the runnable state of the waker thread
      SELECT c.depth + 1, c.waker_utid, prev.waker_utid, prev.ts
      FROM wakeup_chain c
      JOIN thread_state prev ON c.waker_utid = prev.utid AND prev.state = 'R'
        AND prev.ts + prev.dur = c.ts
      WHERE c.depth < 3 AND c.waker_utid IS NOT NULL
    )
    SELECT c.depth, t.name AS thread_name
    FROM wakeup_chain c
    JOIN thread t ON c.utid = t.utid;
    ```

## Analytical Workflow (Standard Operating Procedure)

To ensure accuracy and efficiency, follow these steps:

1. **Research & Dissection:** Identify the core question and required data
   points.
2. **Mandatory Schema Validation:** Search `__intrinsic_stdlib_objects`
   globally across modules, tables, views, functions, table functions, and
   macros. Filter to `exposed = 1`. Use one precise case-insensitive regexp,
   rank qualified-name matches first, and use `LIMIT 10`. Once a candidate is
   found, read its `summary`; it contains all argument, column, return, and
   descriptive metadata.
   - **Intent Check:** You must verify if a stdlib object already provides the
   needed abstraction before drafting manual arithmetic or custom joins.
  - **IMPORTANT:** For overlaps, intersections, or interval boundaries, search
   the global object table before writing custom timestamp arithmetic.
3. **Draft & Validate Loop (Max 3 Iterations):**
  - [ ] **Draft:** Use only verified schemas. Ensure `INCLUDE PERFETTO
    MODULE` is present for non-prelude modules.
  - [ ] **Verify Idempotency:** Use `CREATE OR REPLACE` or `DROP TABLE IF
    EXISTS` for virtual tables.
  - [ ] **Check Precision:** Are ALL columns prefixed with aliases (e.g.,
    `s.name`)? Are you joining on `utid`/`upid`?
  - [ ] **String Matching:** Did you use `=`, `GLOB`, or case-insensitive
    `regexp(..., ..., 'i')` instead of `LIKE`?
  - [ ] **Span Join Check:** If using `SPAN_JOIN`, are tables `PARTITIONED`
    and materialized?
  - [ ] **Execute:** Run against the session:
    `trace_processor query --remote SESSION "QUERY"`.

   **Execution Rules:**
  - **File Usage:** If you must create a SQL file to execute queries (for
    example, due to query length or escaping issues), you must create them
    in the `/tmp/` directory.
  - **Failure Resilience:** Debug and fix SQL syntax and logic errors when
    query fails. Don't simplify the analytical intent to pass validation.
    For example, if requested to calculate an overlap or intersection, you
    must fix the intersection math. Don't substitute with disjoint queries
    (for example, returning independent total durations) as a workaround.
4. **Cleanup & Finalize:**
  - Explicitly return and state the final validated SQL and explain the
    results to the user.
  - **Save an analysis report.** Write a markdown file in the working
    directory (default `perfetto_analysis_report.md`) containing: the
    question investigated, the trace file(s) analyzed, the findings with
    concrete numbers, the final validated queries (so the analysis can be
    re-run), and open questions / next steps. Point the user at it in
    your final message.
  - Before finishing, delete any temporary SQL files created in `/tmp/`.

## Where to look for more

- Language tour:
  <https://perfetto.dev/docs/analysis/perfetto-sql-getting-started>
- Trace processor reference:
  <https://perfetto.dev/docs/analysis/trace-processor>
- Generated table reference:
  <https://perfetto.dev/docs/analysis/sql-tables>
- Generated stdlib reference:
  <https://perfetto.dev/docs/analysis/stdlib-docs>
