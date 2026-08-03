# Debugging Slow PerfettoSQL Queries

This workflow walks an AI agent through diagnosing and fixing a slow
PerfettoSQL query: one that takes tens of seconds, times out, or dominates
an iteration loop. Follow the phases in order — measure before changing
anything, and re-measure after every change.

If you are not familiar with running queries against `trace_processor`,
follow `$SKILL_ROOT/infra-references/querying.md` first, then come back
here.

---

## Phase 1: Eliminate Trace Parsing From the Measurement

The first query against a trace pays the full trace-parse cost (tens of
seconds for multi-GB traces). That is not query slowness, and one-shot
`trace_processor query TRACE_FILE` invocations pay it every time.

1.  Make sure the trace is loaded in a named session (details in
    `$SKILL_ROOT/infra-references/querying.md`):

    ```sh
    trace_processor server unix --name mysession --daemonize TRACE_FILE
    ```

2.  Time the query against the warm session:

    ```sh
    trace_processor query --remote mysession "SQL"
    ```

    The shell prints `Query execution time: N ms` on stderr — that number
    is the query alone, excluding process startup and trace parsing. Use
    it as the measurement.

3.  Record this baseline number. Every fix below must be justified by
    re-running this measurement.

If the slowness came from running one-shot `query TRACE_FILE` invocations
in a loop, stop here: the fix is the session, not rewriting the SQL.

## Phase 2: Diagnose

1.  **Read the query plan.** Prefix the query with `EXPLAIN QUERY PLAN`
    and look at the SQLite plan:

    - `SEARCH ... USING INDEX` on the big tables is good.
    - `SCAN` of a large table (`slice`, `counter`, `sched_slice`,
      `thread_state`, `raw`) is the usual culprit — especially a `SCAN`
      that sits *inside* a join loop, where it re-runs once per outer row.
    - The built-in tables have fast lookup on `id`, and `slice` /
      `counter` are optimized for filters on `ts` and `track_id`. A query
      that filters on neither will scan everything.

2.  **Size every relation in the query.** Run `SELECT COUNT(*)` on each
    table/subquery being joined. Multiply the counts of any two relations
    joined on a non-id condition (string match, inequality, timestamp
    range) — that product is the work SQLite may do. A 100k × 100k
    name-match join is 10^10 comparisons; that is the query's problem,
    not trace_processor.

3.  **Check for repeated evaluation.** A `CREATE VIEW`, a `WITH` CTE, or
    an inline subquery is (in general) re-evaluated wherever it is
    referenced. An expensive view referenced by a join can be recomputed
    per outer row.

## Phase 3: Fix (In Order of Leverage)

Apply one change at a time and re-measure after each.

1.  **Filter early, then join.** Restrict by time range, track, process
    or slice-name *before* the join, not in the final `WHERE`. Materialize
    the filtered set (next step) and join the small results.

2.  **Materialize expensive intermediates.**

    ```sql
    CREATE OR REPLACE PERFETTO TABLE my_filtered AS
    SELECT id, ts, dur, track_id, name FROM slice
    WHERE ts > TRACE_START AND name GLOB 'Choreographer*';
    ```

    Unlike a view or CTE, a Perfetto table is computed once and cached
    for the rest of the session. Anything referenced more than once — or
    referenced inside a join — should be a table.

3.  **Index materialized tables for repeated lookups.** If a materialized
    table is joined or filtered on a column repeatedly:

    ```sql
    CREATE OR REPLACE PERFETTO INDEX my_filtered_track
    ON my_filtered(track_id);
    ```

4.  **Never hand-roll interval overlap joins.** An inequality join like
    `a.ts < b.ts + b.dur AND b.ts < a.ts + a.dur` is O(rows(a) × rows(b)).
    Use purpose-built operators instead, which exploit sorted intervals:

    - Search the stdlib for interval helpers before writing any overlap
      logic (`intervals.overlap`, and the intersect macros discoverable
      via `__intrinsic_stdlib_macros` — see
      `$SKILL_ROOT/infra-references/querying.md`).
    - Or use `SPAN_JOIN` (inputs materialized as Perfetto tables and
      `PARTITIONED` so same-table intervals don't overlap).

5.  **Fix string matching.** `GLOB` instead of `LIKE`, `=` for exact
    matches. If a hot join matches names with a pattern, precompute the
    matching ids into a materialized table once and join on `id` instead
    of re-matching strings per row.

6.  **Prefer the stdlib.** Stdlib modules (e.g. `slices.with_context`,
    `sched.with_context`, `slices.cpu_time`) are written against the
    engine's fast paths. If your slow query re-implements one, switch to
    the module.

## Phase 4: Verify and Report

1.  Re-run the Phase 1 measurement. Also verify the rewritten query is
    still *correct*: same row count as the original (or an explained
    difference) and spot-check a few rows.
2.  Include before/after timings, the final query, and any materialized
    tables it depends on in the analysis report you save at the end of
    the session (see the router's "Finishing any analysis" section).
