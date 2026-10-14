# Migrating to PerfettoSQL Pipe Syntax

This guide is for people who already know PerfettoSQL. It covers why we are
introducing pipe syntax, how it differs from existing macros and virtual
tables, and how to migrate existing queries.

> **Warning:** Pipe syntax is **experimental and unstable**. Do **not** rely on
> it in dashboards, scripts, or production queries. Anything can and will break
> without notice. Outside the PerfettoSQL standard library, you must opt in on
> your connection with `PERFETTO PRAGMA pipelines = 1;`.

Related pages:

- [Getting Started with PerfettoSQL Pipe Syntax](/docs/analysis/perfetto-sql-pipe-getting-started.md)
- [PerfettoSQL Pipe Syntax Reference](/docs/analysis/perfetto-sql-pipe-syntax.md)
- [PerfettoSQL Pipe Operator Implementation Notes](/docs/design-docs/perfetto-sql-pipelines.md)

---

## Why Pipe Syntax?

Writing trace analysis queries in standard SQLite has several recurring pain
points:

1. **Table macros and virtual tables do not compose well.** Operations like
   interval intersection and tree traversal do not exist in SQL. Perfetto
   exposed them through SQLite virtual tables like `SPAN_JOIN` or C++ table
   functions wrapped in macros like `_interval_intersect!`,
   `_graph_aggregating_scan!`, and `_callstacks_self_to_cumulative!`. Chaining
   two or three of these together requires deeply nested subqueries or a sequence
   of intermediate views and tables.
2. **Manual `id` plumbing and back-joins.** `_interval_intersect!` only returns
   `ts`, `dur`, `id_0`, `id_1`, and so on. To read any other columns from your
   inputs, every input needs an `id` column or an `_ii_subquery!` wrapper,
   followed by a `JOIN` per input table on `id_0`, `id_1`, etc.
3. **Manual indexing and temporary tables for trees.** Summing values up a
   callstack or heap graph with `_graph_aggregating_scan!` requires
   materializing a raw table, creating a `PERFETTO INDEX` on `parent_id` so the
   leaf-finding `NOT IN` query does not go quadratic, running the scan, and
   joining the result back onto the raw table by `id`.
4. **Hard-to-read macro errors and SQLite virtual table overhead.** PerfettoSQL
   macros are text substitutions, so a mistake in an argument produces a SQLite
   error pointing inside the expanded macro body. At runtime, passing rows
   between SQLite and custom operators forces every row through SQLite's
   row-at-a-time virtual table interface.

### What changes with pipes

PerfettoSQL pipe syntax uses linear `|>` stages, modeled on
[GoogleSQL pipe syntax](https://cloud.google.com/bigquery/docs/pipe-syntax), and
runs them on a columnar batch executor:

- **`INTERVAL INTERSECTION OF` and `TREE ACCUMULATE` are part of the language.**
  They carry input columns through automatically. You do not need `id_0`/`id_1`
  back-joins, `_ii_subquery!`, or `parent_id` indexes.
- **Direct dataframe scans.** When a pipeline reads a finalized `PERFETTO TABLE`
  such as `slice` or `sched`, it reads the table's columnar storage directly in
  C++ without going through SQLite.
- **Compile-time column checks and pruning.** The compiler checks column names
  and types up front, points at the offending token on error, and drops unused
  columns and unused tree aggregates before execution.

---

## Opting In

In `trace_processor_shell`, the Perfetto UI, the Python API, and diff tests,
enable pipe syntax on your connection with:

```sql
PERFETTO PRAGMA pipelines = 1;
```

You can turn it back off with `PERFETTO PRAGMA pipelines = 0;`. Inside the
PerfettoSQL standard library, pipelines are enabled by default without the
pragma.

---

## What Goes in SQL vs. the Pipeline?

Pipe syntax is being built incrementally. Today, pipelines support three trace
operations, `INTERVAL INTERSECTION OF`, `INTERVAL FLATTEN`, and
`TREE ACCUMULATE`, and six
column-reshaping stages: `SELECT`, `EXTEND`, `DROP`, `RENAME`, `SET`, and `AS`.

When migrating a query today, split the work between SQL and the pipeline as
follows:

| Task | Where to do it today |
| :--- | :--- |
| Filtering rows, standard `JOIN`s, `GROUP BY`, window functions, and computed expressions | Inside the `FROM (SELECT ...)` source subquery, or in a standard SQL query over the resulting `PERFETTO TABLE`. |
| Intersecting time intervals across two or more tables | `INTERVAL INTERSECTION OF (...) [PER ...]` at the start of a pipeline. |
| Counting or summing concurrent activity within one set of intervals | `\|> INTERVAL FLATTEN [PER ...] AGGREGATE COUNT(*) AS active` stage; see the [worked example](/docs/analysis/perfetto-sql-pipe-getting-started.md#flattening-overlapping-intervals-interval-flatten). |
| Summing values up or down a parent-child tree | `\|> TREE ACCUMULATE UP \| DOWN SUM(col) AS total` stage. |
| Picking, dropping, renaming, or swapping columns | `\|> SELECT`, `\|> EXTEND`, `\|> DROP`, `\|> RENAME`, `\|> SET`, `\|> AS` stages. |
| Sorting final output by numeric columns | `\|> ORDER BY col [DESC]` as the last stage. |
| Sorting by strings or expressions, or limiting rows | In a `SELECT` with `ORDER BY` or `LIMIT` over the `PERFETTO TABLE` created by the pipeline. |

> **Note:** A pipeline can be run as a top-level statement or as the body of
> `CREATE [OR REPLACE] PERFETTO TABLE ... AS <pipeline>`. It cannot yet be used
> as the body of `CREATE PERFETTO VIEW`, `CREATE PERFETTO FUNCTION`, or a `WITH`
> CTE.

---

## Migrating Interval Intersections

Replace `_interval_intersect!` and `SPAN_JOIN` with `INTERVAL INTERSECTION OF`.

### Before: `_interval_intersect!`

```sql
INCLUDE PERFETTO MODULE intervals.intersect;

CREATE PERFETTO TABLE _estimates_w_tasks_attribution AS
SELECT ii.ts, ii.dur, ii.cpu, uw.estimated_mw, s.utid
FROM _interval_intersect!(
  (
    _ii_subquery!(_unioned_wattson_estimates_mw),
    _ii_subquery!(_wattson_task_slices)
  ),
  (cpu)
) AS ii
JOIN _unioned_wattson_estimates_mw AS uw
  ON uw._auto_id = id_0
JOIN _wattson_task_slices AS s
  ON s._auto_id = id_1;
```

### After: `INTERVAL INTERSECTION OF`

```sql
CREATE PERFETTO TABLE _estimates_w_tasks_attribution AS
INTERVAL INTERSECTION OF (
  _unioned_wattson_estimates_mw AS estimate,
  _wattson_task_slices AS task
) PER cpu
|> SELECT ts, dur, cpu, estimate.estimated_mw, task.utid;
```

### Migration checklist for `INTERVAL INTERSECTION OF`

1. **Drop the back-joins and `_ii_subquery!` wrappers.**
   `_interval_intersect!` required every input to have an `id` column so you
   could join `id_0` and `id_1` back to the input tables.
   `INTERVAL INTERSECTION OF` carries the operands' columns automatically and
   only requires `ts` and `dur` on its inputs. Only keep `_ii_subquery!` if you
   specifically need SQLite's `_auto_id` exposed as `alias.id` in your output.
2. **Give every operand a distinct alias.**
   Write `INTERVAL INTERSECTION OF (a AS x, b AS y)`. Omitting `AS <alias>` on
   a subquery or reusing an alias across operands is a compile error.
3. **Use `PER` instead of partition column lists or `PARTITIONED`.**
   Write `PER cpu` or `PER utid, track_id`. For unpartitioned intersections,
   omit `PER` altogether.
4. **Qualify operand columns, and leave region `ts`, `dur`, and `PER` columns bare.**
   Bare `ts` and `dur` are the intersected region's start and duration. Bare
   `cpu` is the partition key shared by the matched rows. All other operand
   columns, including each operand's original unclipped `x.ts` and `x.dur`, must
   be qualified with the operand's alias, such as `x.utid` or `y.freq`. Always
   follow `INTERVAL INTERSECTION OF` with `|> SELECT ...`; without a `SELECT`
   stage, every operand's `ts` and `dur` remain in the output row and produce
   duplicate column names.
5. **Watch for behavioral differences.**
   - **Rows with `dur IS NULL` or `ts IS NULL` are skipped.** If an input table
     computes duration with `lead(ts) OVER (...) - ts AS dur`, its final row has
     `dur IS NULL`. `_interval_intersect!` coerced `NULL` duration to `0`;
     `INTERVAL INTERSECTION OF` skips rows where `ts` or `dur` is `NULL`.
   - **Negative `ts` or `dur` is an error.** Unfinished slices with `dur = -1`
     are rejected with `ts or dur is below zero`. Filter `WHERE dur >= 0` or
     `WHERE dur > 0` in the input.
   - **Overlapping intervals within an operand work.** `SPAN_JOIN` produces
     wrong results when intervals overlap within a single table, whereas
     `INTERVAL INTERSECTION OF` handles overlapping intervals automatically.

---

## Migrating Tree Aggregations

Replace `_graph_aggregating_scan!`, `_callstacks_self_to_cumulative!`, and
recursive CTE subtree or path sums with `TREE ACCUMULATE UP` and
`TREE ACCUMULATE DOWN`.

### Before: `_graph_aggregating_scan!`

```sql
CREATE PERFETTO TABLE _heap_graph_class_tree_cumulatives AS
SELECT
  a.id,
  a.cumulative_count,
  a.cumulative_size
FROM _graph_aggregating_scan!(
  (
    SELECT id AS source_node_id, parent_id AS dest_node_id
    FROM _heap_graph_class_tree
    WHERE parent_id IS NOT NULL
  ),
  (
    SELECT
      id,
      self_count AS cumulative_count,
      self_size AS cumulative_size
    FROM _heap_graph_class_tree
    WHERE id NOT IN (
      SELECT parent_id FROM _heap_graph_class_tree WHERE parent_id IS NOT NULL
    )
  ),
  (cumulative_count, cumulative_size),
  (
    WITH agg AS (
      SELECT
        t.id,
        SUM(t.cumulative_count) AS child_count,
        SUM(t.cumulative_size) AS child_size
      FROM $table t
      GROUP BY t.id
    )
    SELECT
      a.id,
      a.child_count + r.self_count AS cumulative_count,
      a.child_size + r.self_size AS cumulative_size
    FROM agg a
    JOIN _heap_graph_class_tree r USING (id)
  )
) AS a;
```

### After: `TREE ACCUMULATE UP`

```sql
CREATE PERFETTO TABLE _heap_graph_class_tree_cumulatives AS
FROM _heap_graph_class_tree
|> TREE ACCUMULATE UP
  SUM(self_count) AS cumulative_count,
  SUM(self_size) AS cumulative_size;
```

### Migration checklist for `TREE ACCUMULATE`

1. **Delete intermediate raw tables, `parent_id` indexes, and back-joins.**
   You can pipe directly from a macro or subquery:
   ```sql
   CREATE PERFETTO TABLE _linux_perf_callstacks AS
   FROM _callstacks_for_callsites!((SELECT callsite_id FROM perf_sample))
   |> TREE ACCUMULATE UP SUM(self_count) AS cumulative_count;
   ```
   `TREE ACCUMULATE` keeps all columns of the input row and appends
   `cumulative_count` at the end. You do not need to `JOIN` the result back to
   the input table or create a `PERFETTO INDEX` on `parent_id`.
2. **Make sure the source has `id` and `parent_id` columns.**
   `TREE ACCUMULATE` looks up bare `id` and `parent_id` in the current row.
   Root nodes must have `parent_id IS NULL`.
3. **Totals always include the node itself.**
   `TREE ACCUMULATE UP SUM(x) AS total` adds `x` for the node to `x` for all of
   its descendants. `TREE ACCUMULATE DOWN SUM(x) AS path` adds `x` for the node
   to `x` for all of its ancestors. When migrating hand-written
   `_graph_aggregating_scan!` queries, check whether the old step query forgot
   to add `r.self_*` for non-leaf nodes.
4. **Preserve `ORDER BY id` on public tables if required.**
   `TREE ACCUMULATE UP` emits rows in child-first order, and `DOWN` emits rows
   in parent-first order. If a public stdlib table or test expects rows sorted
   by `id`, end the pipeline with `|> ORDER BY id`.

---

## Common Pitfalls

### 1. Every source column needs a valid, distinct name

When a pipeline reads from a SQL subquery, the compiler checks every column
returned by that subquery before column pruning runs, including columns that a
later stage drops:

- **Unaliased expressions fail:**
  ```sql
  -- ERROR: expected every column to have a valid name, but '1 + 1' is not one:
  -- give it one with AS
  FROM (SELECT id, parent_id, 1 + 1 FROM tree)
  |> TREE ACCUMULATE UP SUM(id) AS total
  |> SELECT id, total;
  ```
- **Duplicate column names in a subquery fail:** SQLite renames the second `x`
  in `SELECT 1 AS x, 2 AS x` to `x:1`. The pipeline compiler rejects this with:
  `expected distinct column names, but there are two named 'x', which SQLite renamed to 'x' and 'x:1'`.

### 2. Wrap `WHERE` and `JOIN` in parentheses inside `FROM`

A pipeline's `FROM` clause only accepts a table or view name, or a parenthesized
`SELECT` subquery:

```sql
-- ERROR:
FROM slice WHERE dur > 0 |> TREE ACCUMULATE UP SUM(dur) AS total;

-- OK:
FROM (SELECT id, parent_id, dur FROM slice WHERE dur > 0)
|> TREE ACCUMULATE UP SUM(dur) AS total;
```

### 3. Table aliases snapshot the row at the point they are bound

In pipe stages like `DROP`, `RENAME`, `SET`, and `EXTEND`, a table alias
remembers the columns the row had when the alias was created:

- `FROM t |> DROP x |> SELECT t.x` still works and recovers `x`.
- `FROM t |> RENAME x AS y |> SELECT t.x` still accesses the column as `t.x`,
  while bare `y` accesses it by its new name.
- `|> SELECT ...` builds a new row and clears all previous aliases.
- `|> AS u` replaces all previous aliases with `u`, snapshotting the current
  row.
