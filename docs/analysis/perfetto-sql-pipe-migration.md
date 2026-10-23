# Migrating to PerfettoSQL Pipe Syntax

This guide is for people who already know PerfettoSQL. It covers why pipe
syntax exists, how it differs from the macros and virtual tables it replaces,
and how to migrate queries to it.

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

1. **Macros and virtual tables do not compose.** SQL has no interval
   intersection or tree traversal, so Perfetto exposes them as SQLite virtual
   tables like `SPAN_JOIN`, or as C++ table functions wrapped in macros like
   `_interval_intersect!` and `_graph_aggregating_scan!`. Chaining two or
   three of them takes deeply nested subqueries or a series of intermediate
   tables.
2. **Back-joins on `id`.** `_interval_intersect!` returns only `ts`, `dur`,
   the partition columns and `id_0`, `id_1`, and so on. To read any other
   column, every input needs an `id` column or an `_ii_subquery!` wrapper, and
   the result needs a `JOIN` back to each input.
3. **Indexes and temporary tables for trees.** Summing up a callstack or heap
   graph with `_graph_aggregating_scan!` means saving a table, indexing
   `parent_id` so the leaf-finding `NOT IN` query is not quadratic, running the
   scan, and joining the result back by `id`.
4. **Confusing errors and slow row-by-row execution.** Macros are text
   substitution, so a mistake in an argument gives a SQLite error pointing
   inside the expanded macro. At runtime, every row passes one at a time
   through SQLite's virtual table interface.

### What changes with pipes

Pipe syntax chains `|>` stages, as
[GoogleSQL pipe syntax](https://cloud.google.com/bigquery/docs/pipe-syntax)
does, and runs them on a columnar executor in C++:

- **Interval and tree operations are part of the language.** Stages like
  `INTERVAL INTERSECTION OF` and `TREE ACCUMULATE` carry input columns through,
  so there are no `id_0`/`id_1` back-joins, `_ii_subquery!` wrappers or
  `parent_id` indexes.
- **Tables are read directly.** A pipeline reads tables such as `slice`,
  `sched` or any `PERFETTO TABLE` from their columnar storage, without going
  through SQLite.
- **Errors come early.** Column names and types are checked before anything
  runs, and errors point at the token at fault. Unused columns and aggregates
  are dropped before execution.

---

## Opting In

In `trace_processor_shell`, the Perfetto UI, the Python API and diff tests,
turn pipe syntax on with:

```sql
PERFETTO PRAGMA pipelines = 1;
```

`PERFETTO PRAGMA pipelines = 0;` turns it off again. The standard library
can always use pipelines.

---

## What Goes in SQL vs. the Pipeline?

Pipelines do not do everything SQL does yet. Split the work like this:

| Task | Where to do it today |
| :--- | :--- |
| Filtering by comparing columns with numbers, joined by `AND` | `\|> WHERE dur > 0 AND depth = 0` |
| Any other filter, `JOIN`s, window functions and computed expressions | In the `FROM (SELECT ...)` subquery, or in SQL over the table the pipeline saves. |
| Intersecting intervals across two or more tables | `INTERVAL INTERSECTION OF (...) [PER ...]` |
| Counting or summing concurrent activity in one set of intervals | `\|> INTERVAL FLATTEN [PER ...] AGGREGATE ...`; see the [example](/docs/analysis/perfetto-sql-pipe-getting-started.md#flattening-overlapping-intervals-interval-flatten). |
| Filling the gaps between intervals | `\|> INTERVAL FILL GAPS WITH <background> [PER ...]` |
| Turning intervals into a slice tree by an overlap predicate | `\|> TREE FROM INTERVALS ON <condition>` |
| Aggregating up or down a tree | `\|> TREE ACCUMULATE UP \| DOWN ...` |
| `COUNT(*)`, and `SUM`, `MIN` and `MAX` of integer columns, per group (`FIRST` and `LAST` in `TREE ACCUMULATE DOWN`) | `\|> AGGREGATE ... [GROUP BY ...]` |
| Any other aggregate | In SQL. |
| Removing repeated rows | `\|> DISTINCT` |
| Picking, dropping, renaming or swapping columns | `\|> SELECT`, `EXTEND`, `DROP`, `RENAME`, `SET` and `AS` |
| Sorting by numeric columns | `\|> ORDER BY col [DESC]` |
| Sorting by strings or expressions, or `LIMIT` | In SQL over the table the pipeline saves. |

A pipeline can be a statement on its own or the body of
`CREATE [OR REPLACE] PERFETTO TABLE`. It cannot yet be the body of a view,
function or `WITH` clause.

---

## Migrating Interval Intersections

Replace `_interval_intersect!` and `SPAN_JOIN` with `INTERVAL INTERSECTION OF`.

### Before: `_interval_intersect!`

```sql
INCLUDE PERFETTO MODULE intervals.intersect;
INCLUDE PERFETTO MODULE wattson.utils;

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

1. **Drop the back-joins and `_ii_subquery!` wrappers.** Inputs only need
   `ts` and `dur`; every other column carries through.
2. **Give every input its own alias:** `INTERVAL INTERSECTION OF (a AS x, b AS
   y)`. A subquery without an alias, or two inputs with one alias, is an error.
3. **Use `PER` for partitions:** `PER cpu`, or `PER utid, track_id`. Leave it
   out to intersect everything.
4. **Bare names mean the region.** `ts` and `dur` are the overlapping region,
   and `cpu` is the shared `PER` value. An input's own bounds are `x.ts` and
   `x.dur`. Other columns can be bare, as in `utid`, unless two inputs have
   one of that name: then write `x.utid`. If two inputs share a column name,
   end with a `|> SELECT` which names each copy, or the result cannot be saved
   as a table.
5. **Watch for differences in behaviour.**
   - **`NULL` bounds are skipped.** A `dur` computed with
     `lead(ts) OVER (...) - ts` is `NULL` on the last row.
     `_interval_intersect!` treated that as `dur = 0`; the pipeline skips the
     row.
   - **Intervals may overlap within an input.** `SPAN_JOIN` gives wrong
     results when they do; `INTERVAL INTERSECTION OF` handles it.
   - **Negative bounds are an error**, as in `_interval_intersect!`. Filter
     out unfinished slices, which have `dur = -1`, with `|> WHERE dur >= 0`
     or in the input subquery.

---

## Migrating Tree Aggregations

Replace `_graph_aggregating_scan!`, and recursive CTEs which sum over subtrees
or paths, with `TREE ACCUMULATE UP` and `TREE ACCUMULATE DOWN`.

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

1. **Delete intermediate tables, `parent_id` indexes and back-joins.** Pipe
   straight from a macro or subquery:
   ```sql
   CREATE PERFETTO TABLE _linux_perf_callstacks AS
   FROM _callstacks_for_callsites!((SELECT callsite_id FROM perf_sample))
   |> TREE ACCUMULATE UP SUM(self_count) AS cumulative_count;
   ```
   The input's columns are kept, with `cumulative_count` added at the end.
2. **The source needs `id` and `parent_id` columns**, with `parent_id IS NULL`
   on roots.
3. **Totals include the node itself.** `UP SUM(x)` adds a node's `x` to its
   descendants'; `DOWN SUM(x)` adds it to its ancestors'. Hand-written
   `_graph_aggregating_scan!` steps sometimes forgot to add `r.self_*` for
   nodes with children, so check that results match.
4. **Sort if order matters.** `UP` outputs children first and `DOWN` parents
   first. If a table or test expects rows by `id`, end with `|> ORDER BY id`.

---

## Common Pitfalls

### 1. Every source column needs a valid, distinct name

Every column of a source subquery is checked, even one a later stage drops:

- **An expression needs a name:**
  ```sql
  -- ERROR: expected every column to have a name, but column 3 has none:
  -- give it one with AS
  FROM (SELECT id, parent_id, 1 + 1 FROM tree)
  |> TREE ACCUMULATE UP SUM(id) AS total
  |> SELECT id, total;
  ```
- **Names must be distinct:** `FROM (SELECT 1 AS x, 2 AS x)` fails with
  `expected distinct column names, but there are two named 'x'`.

### 2. `FROM` takes a table, a macro or a subquery

`FROM` cannot have its own `WHERE` or `JOIN`. Filter with a `|> WHERE` stage,
or put the SQL in a subquery:

```sql
-- ERROR:
FROM slice WHERE dur > 0 |> TREE ACCUMULATE UP SUM(dur) AS total;

-- OK:
FROM slice
|> WHERE dur > 0
|> TREE ACCUMULATE UP SUM(dur) AS total;

-- OK, and needed for a JOIN or a filter WHERE does not support:
FROM (SELECT id, parent_id, dur FROM slice WHERE name GLOB 'Choreographer*')
|> TREE ACCUMULATE UP SUM(dur) AS total;
```

### 3. An alias remembers the row as it was when it was set

`DROP`, `RENAME`, `SET` and `EXTEND` do not change an alias:

- `FROM t AS t |> DROP x |> SELECT t.x` still finds `x`.
- After `FROM t AS t |> RENAME x AS y`, the column is `y` bare but still `t.x`
  through the alias.
- `|> SELECT` clears every alias.
- `|> AS u` replaces every alias with `u`, for the row as it is now.
