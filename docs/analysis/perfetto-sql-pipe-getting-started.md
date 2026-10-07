# Getting Started with PerfettoSQL Pipe Syntax

This page is a prototype of the introductory guide to querying traces with
PerfettoSQL pipe syntax.

> **Warning:** Pipe syntax is **experimental and unstable**. Do **not** rely on
> it in dashboards, scripts, or production queries. Anything can and will break
> without notice. To run the examples on this page in the Perfetto UI or
> `trace_processor_shell`, start your query with `PERFETTO PRAGMA pipelines = 1;`.

Related pages:

- [Migrating to PerfettoSQL Pipe Syntax](/docs/analysis/perfetto-sql-pipe-migration.md)
- [PerfettoSQL Pipe Syntax Reference](/docs/analysis/perfetto-sql-pipe-syntax.md)
- [PerfettoSQL Pipe Operator Implementation Notes](/docs/design-docs/perfetto-sql-pipelines.md)

---

## How Pipelines Work

Standard SQL queries read inside-out: you start with `SELECT` to list the
columns you want at the end, then write `FROM` to say where the data comes
from, and nest subqueries or CTEs whenever you need multi-step transformations.

With pipe syntax, queries read top-to-bottom in the order data is processed:

1. Start with a **source**: `FROM <table>`, `FROM (<subquery>)`, or
   `INTERVAL INTERSECTION OF (...)`.
2. Chain one or more **pipe stages** with `|>` that transform, aggregate, or
   reshape the rows step by step.

```sql
PERFETTO PRAGMA pipelines = 1;

FROM slice
|> RENAME dur AS duration_ns
|> SELECT id, name, ts, duration_ns;
```

Every `|>` stage takes the table produced by the previous step and outputs a new
table for the next step.

### Saving a pipeline as a table

You can run a pipeline directly as a query, or save its output into a
`PERFETTO TABLE` so you can query it further with SQL:

```sql
PERFETTO PRAGMA pipelines = 1;

CREATE PERFETTO TABLE named_slices AS
FROM (SELECT id, ts, dur, name FROM slice WHERE dur > 0)
|> RENAME dur AS duration_ns;

SELECT name, duration_ns
FROM named_slices
ORDER BY duration_ns DESC
LIMIT 10;
```

---

## Intersecting Time Intervals: `INTERVAL INTERSECTION OF`

Many trace analysis questions ask where two or more sets of time intervals
`[ts, ts + dur)` overlap:

- What CPU frequency was each core running at during every scheduling slice?
- Which slices were active while a thread was in a particular state?
- How much power was used by each task during active GPU regions?

`INTERVAL INTERSECTION OF` takes two or more interval sources and emits one row
for every overlapping time region they share:

```sql
PERFETTO PRAGMA pipelines = 1;

-- 1. Scheduling slices with positive duration
CREATE PERFETTO TABLE running_sched AS
SELECT ts, dur, cpu, utid
FROM sched
WHERE dur > 0;

-- 2. CPU frequency intervals per CPU
CREATE PERFETTO TABLE cpu_freq AS
SELECT
  c.ts,
  lead(c.ts) OVER (PARTITION BY t.cpu ORDER BY c.ts) - c.ts AS dur,
  t.cpu,
  c.value AS freq
FROM counter AS c
JOIN cpu_counter_track AS t ON c.track_id = t.id
WHERE t.name = 'cpufreq';

-- 3. Intersect scheduling slices with CPU frequency per CPU
CREATE PERFETTO TABLE sched_with_freq AS
INTERVAL INTERSECTION OF (running_sched AS s, cpu_freq AS f) PER cpu
|> SELECT ts, dur, cpu, s.utid, f.freq;
```

### How it works

```
running_sched (s):   [------- utid 10 -------)     [--- utid 20 ---)
cpu_freq (f):        [--- 1.8 GHz ---)[-------- 2.4 GHz --------)
                     -----------------------------------------------
Result regions:      [-- 10 @ 1.8G --)[- 10 -]     [-- 20 @ 2.4G --)
```

- **Region `ts` and `dur`:** Bare `ts` and `dur` in the output are the start and
  duration of the overlapping region, clipped to where all inputs were active at
  the same time.
- **Partitioning with `PER`:** Adding `PER cpu` intersects intervals only when
  they occur on the same `cpu`. The `cpu` column is available directly by its
  bare name.
- **Accessing input columns:** Each input has an alias, here `AS s` and `AS f`.
  All columns from `s` and `f` are carried along and accessed via `s.<col>` and
  `f.<col>`. This includes each input's original unclipped `s.ts` and `s.dur`.

---

## Flattening Overlapping Intervals: `INTERVAL FLATTEN`

To count concurrent activity or sum a value across active intervals, use
`INTERVAL FLATTEN`. It splits a single set of overlapping intervals into
segments and computes an aggregate for each segment.

For example, two tasks run during `[0, 10)` and `[5, 15)`, with weights 2 and
3. Flattening shows when one or both are active:

```sql
PERFETTO PRAGMA pipelines = 1;

FROM (
  SELECT 0 AS ts, 10 AS dur, 2 AS weight
  UNION ALL
  SELECT 5 AS ts, 10 AS dur, 3 AS weight
)
|> INTERVAL FLATTEN AGGREGATE COUNT(*) AS active, SUM(weight) AS total_weight;
```

| ts | dur | active | total_weight |
| -- | --- | ------ | ------------ |
| 0 | 5 | 1 | 2 |
| 5 | 5 | 2 | 5 |
| 10 | 5 | 1 | 3 |

Use `PER` to flatten each group independently. For example, count overlapping
positive-duration slices on each track:

```sql
FROM (SELECT ts, dur, track_id FROM slice WHERE dur > 0)
|> INTERVAL FLATTEN PER track_id AGGREGATE COUNT(*) AS active;
```

The result contains `ts`, `dur`, `track_id`, and `active`. Other input columns
are discarded. Gaps with no activity produce no rows. Input need not be sorted.

`SUM` adds the values of active rows; it does not multiply them by the segment's
duration. To calculate a duration-weighted total, save the pipeline as a
`PERFETTO TABLE` and aggregate `dur * total_weight` in a SQL query.

See the [syntax reference](/docs/analysis/perfetto-sql-pipe-syntax.md#interval-flatten)
for point intervals, `NULL` handling, and supported aggregate expressions.

---

## Summing Over Trees: `TREE ACCUMULATE`

Traces often contain trees where each row has an `id` and a `parent_id`, with
`parent_id IS NULL` at the roots:

- Nested userspace slices in `slice`
- Sampled callstacks from CPU profiling, perf, and native heap profiles
- Java heap graph shortest-path trees

`TREE ACCUMULATE` computes cumulative sums over an `id` and `parent_id` tree in
a single stage, keeping all existing columns and appending the totals:

- **`TREE ACCUMULATE UP`:** Sums each node's value with all of its descendants.
- **`TREE ACCUMULATE DOWN`:** Sums each node's value with all of its ancestors
  from the root down to the node.

```sql
PERFETTO PRAGMA pipelines = 1;

CREATE PERFETTO TABLE tree AS
SELECT 0 AS id, NULL AS parent_id, 'root' AS name, 10 AS self
UNION ALL SELECT 1, 0, 'a', 20
UNION ALL SELECT 2, 0, 'b', 30
UNION ALL SELECT 3, 1, 'c', 40;

CREATE PERFETTO TABLE tree_totals AS
FROM tree
|> TREE ACCUMULATE UP SUM(self) AS subtree_total
|> TREE ACCUMULATE DOWN SUM(self) AS path_total;

SELECT name, self, subtree_total, path_total
FROM tree_totals
ORDER BY id;
```

For the tree `root (10) -> a (20) -> c (40)` and `root (10) -> b (30)`:

| `name` | `self` | `subtree_total` | `path_total` |
| :--- | ---: | ---: | ---: |
| `root` | 10 | 100 | 10 |
| `a` | 20 | 60 | 30 |
| `b` | 30 | 30 | 40 |
| `c` | 40 | 40 | 70 |

---

## Reshaping Columns

Instead of wrapping queries in outer `SELECT`s just to add, drop, or rename a
column, you can modify the row in place with column-reshaping stages:

- **`|> SELECT`:** Pick, reorder, or rename columns. You can also use
  `* EXCEPT (col1, col2)` to exclude columns or `* REPLACE (other_col AS col1)`
  to substitute a column in place:
  ```sql
  FROM slice
  |> SELECT * EXCEPT (arg_set_id, parent_id)
  ```
- **`|> EXTEND`:** Append new columns to the end of the current row while
  keeping all existing columns:
  ```sql
  FROM slice AS s
  |> EXTEND s.dur AS original_dur
  ```
- **`|> DROP`:** Remove one or more columns by name:
  ```sql
  FROM slice
  |> DROP category, track_id, arg_set_id
  ```
- **`|> RENAME`:** Rename columns in place without listing every other column:
  ```sql
  FROM slice
  |> RENAME ts AS start_ts, dur AS duration_ns
  ```
- **`|> SET`:** Replace an existing column's value in place:
  ```sql
  FROM tree AS t
  |> TREE ACCUMULATE UP SUM(self) AS total
  |> SET self = total
  |> DROP total
  ```

---

## Next Steps

- [PerfettoSQL Pipe Syntax Reference](/docs/analysis/perfetto-sql-pipe-syntax.md)
- [Migrating to PerfettoSQL Pipe Syntax](/docs/analysis/perfetto-sql-pipe-migration.md)
- [PerfettoSQL Pipe Operator Implementation Notes](/docs/design-docs/perfetto-sql-pipelines.md)
