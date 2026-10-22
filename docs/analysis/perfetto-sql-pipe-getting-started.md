# Getting Started with PerfettoSQL Pipe Syntax

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

A SQL query reads inside-out: `SELECT` comes first but runs last, and every
extra step needs another subquery or CTE. A pipeline reads top to bottom, in
the order the work happens:

1. It starts with a **source**: `FROM <table>`, `FROM (<subquery>)`, or
   `INTERVAL INTERSECTION OF (...)`.
2. Each `|>` **stage** takes the rows from the step before and passes new rows
   to the next.

```sql
PERFETTO PRAGMA pipelines = 1;

FROM slice
|> WHERE dur > 0
|> RENAME dur AS duration_ns
|> SELECT id, name, ts, duration_ns;
```

### Saving a pipeline as a table

Run a pipeline as a query on its own, or save it as a `PERFETTO TABLE` and query
that with SQL:

```sql
PERFETTO PRAGMA pipelines = 1;

CREATE PERFETTO TABLE named_slices AS
FROM slice
|> WHERE dur > 0
|> SELECT id, ts, dur AS duration_ns, name;

SELECT name, duration_ns
FROM named_slices
ORDER BY duration_ns DESC
LIMIT 10;
```

---

## Intersecting Time Intervals: `INTERVAL INTERSECTION OF`

Many questions about a trace ask where two or more sets of intervals
`[ts, ts + dur)` overlap:

- What CPU frequency was each core running at during every scheduling slice?
- Which slices were active while a thread was in a particular state?
- How much power did each task use while the GPU was active?

`INTERVAL INTERSECTION OF` takes two or more interval sources and gives one row
for every region where all of them overlap:

```sql
PERFETTO PRAGMA pipelines = 1;

CREATE PERFETTO TABLE running_sched AS
SELECT ts, dur, cpu, utid
FROM sched
WHERE dur > 0;

CREATE PERFETTO TABLE cpu_freq AS
SELECT
  c.ts,
  lead(c.ts) OVER (PARTITION BY t.cpu ORDER BY c.ts) - c.ts AS dur,
  t.cpu,
  c.value AS freq
FROM counter AS c
JOIN cpu_counter_track AS t ON c.track_id = t.id
WHERE t.name = 'cpufreq';

CREATE PERFETTO TABLE sched_with_freq AS
INTERVAL INTERSECTION OF (running_sched AS s, cpu_freq AS f) PER cpu
|> SELECT ts, dur, cpu, utid, freq;
```

```
running_sched (s):   [------- utid 10 -------)     [--- utid 20 ---)
cpu_freq (f):        [--- 1.8 GHz ---)[-------- 2.4 GHz --------)
                     -----------------------------------------------
Result regions:      [-- 10 @ 1.8G --)[- 10 -]     [-- 20 @ 2.4G --)
```

- **`ts` and `dur`** are the overlapping region, clipped to where every input
  is active.
- **`PER cpu`** only intersects rows on the same `cpu`.
- **Input columns** carry through. Write them bare, as in `utid` and `freq`,
  or through the input's alias, as in `s.utid`. If two inputs have a column of
  the same name, the alias is needed.
- **An input's own bounds** are `s.ts` and `s.dur`, before clipping.

---

## Flattening Overlapping Intervals: `INTERVAL FLATTEN` {#flattening-overlapping-intervals-interval-flatten}

`INTERVAL FLATTEN` splits one set of overlapping intervals into segments and
aggregates the rows active in each. Use it to count concurrent activity or sum
a value across it.

Two tasks run during `[0, 10)` and `[5, 15)`, with weights 2 and 3:

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

`PER` flattens each group on its own. To count overlapping slices on each
track:

```sql
FROM slice
|> WHERE dur > 0
|> INTERVAL FLATTEN PER track_id AGGREGATE COUNT(*) AS active;
```

The result has `ts`, `dur`, `track_id` and `active`; every other column is
dropped. Gaps with no activity give no rows.

`SUM` adds the values of the active rows. It does not weight them by the
segment's duration: for that, save the pipeline as a table and sum
`dur * total_weight` in SQL.

See the [syntax reference](/docs/analysis/perfetto-sql-pipe-syntax.md#interval-flatten)
for zero-duration intervals, `NULL`s and the supported aggregates.

---

## Filling Gaps: `INTERVAL FILL GAPS` {#filling-gaps-interval-fill-gaps}

`INTERVAL FILL GAPS` keeps every input row and adds a row for each stretch of a
background interval which no input covers. A filler row takes its values from
the background, and `NULL` for columns the background does not have.

To mark when each CPU was idle between 0 and 10:

```sql
PERFETTO PRAGMA pipelines = 1;

CREATE PERFETTO TABLE spans AS
SELECT 2 AS ts, 3 AS dur, 0 AS cpu, 'run' AS state
UNION ALL SELECT 6, 2, 0, 'run'
UNION ALL SELECT 1, 4, 1, 'run';

FROM spans
|> INTERVAL FILL GAPS WITH (SELECT 0 AS ts, 10 AS dur, 'idle' AS state) PER cpu
|> ORDER BY cpu, ts;
```

| ts | dur | cpu | state |
| -- | --- | --- | ----- |
| 0 | 2 | 0 | idle |
| 2 | 3 | 0 | run |
| 5 | 1 | 0 | idle |
| 6 | 2 | 0 | run |
| 8 | 2 | 0 | idle |
| 0 | 1 | 1 | idle |
| 1 | 4 | 1 | run |
| 5 | 5 | 1 | idle |

---

## Summing Over Trees: `TREE ACCUMULATE`

Traces hold many trees where each row has an `id` and a `parent_id`, and roots
have `parent_id IS NULL`:

- Nested slices in `slice`.
- Callstacks from CPU profiling, perf and native heap profiles.
- Java heap graph shortest-path trees.

`TREE ACCUMULATE` sums over such a tree in one stage. It keeps every column and
adds the totals at the end:

- **`UP`** sums each node with all its descendants.
- **`DOWN`** sums each node with all its ancestors.

```sql
PERFETTO PRAGMA pipelines = 1;

CREATE PERFETTO TABLE tree AS
SELECT 0 AS id, NULL AS parent_id, 'root' AS name, 10 AS self
UNION ALL SELECT 1, 0, 'a', 20
UNION ALL SELECT 2, 0, 'b', 30
UNION ALL SELECT 3, 1, 'c', 40;

FROM tree
|> TREE ACCUMULATE UP SUM(self) AS subtree_total
|> TREE ACCUMULATE DOWN SUM(self) AS path_total
|> ORDER BY id
|> SELECT name, self, subtree_total, path_total;
```

For the tree `root (10) -> a (20) -> c (40)` and `root (10) -> b (30)`:

| `name` | `self` | `subtree_total` | `path_total` |
| :--- | ---: | ---: | ---: |
| `root` | 10 | 100 | 10 |
| `a` | 20 | 60 | 30 |
| `b` | 30 | 30 | 40 |
| `c` | 40 | 40 | 70 |

---

## Grouping: `AGGREGATE`

`AGGREGATE` collapses each group of rows into one, as `GROUP BY` does in SQL:

```sql
FROM slice
|> WHERE dur > 0
|> AGGREGATE COUNT(*) AS slices, SUM(dur) AS total, MAX(dur) AS longest
   GROUP BY track_id;
```

The result has `track_id`, `slices`, `total` and `longest`. Without `GROUP BY`,
every row goes into one group.

---

## Filtering and Sorting Rows

- **`|> WHERE`** keeps the rows that match. Today it supports comparing a
  column with a number, joined with `AND`:
  ```sql
  FROM slice
  |> WHERE dur > 0 AND depth = 0
  ```
- **`|> DISTINCT`** drops repeated rows:
  ```sql
  FROM slice
  |> SELECT track_id
  |> DISTINCT
  ```
- **`|> ORDER BY`** sorts by numeric columns:
  ```sql
  FROM slice
  |> ORDER BY dur DESC
  ```

---

## Reshaping Columns

These stages change the columns without wrapping the query in another
`SELECT`:

- **`|> SELECT`** picks, reorders and renames columns. `* EXCEPT (...)` drops
  some, and `* REPLACE (other_col AS col)` swaps one in place:
  ```sql
  FROM slice
  |> SELECT * EXCEPT (arg_set_id, parent_id)
  ```
- **`|> EXTEND`** adds columns at the end:
  ```sql
  FROM slice AS s
  |> EXTEND s.dur AS original_dur
  ```
- **`|> DROP`** removes columns:
  ```sql
  FROM slice
  |> DROP category, track_id, arg_set_id
  ```
- **`|> RENAME`** renames columns in place:
  ```sql
  FROM slice
  |> RENAME ts AS start_ts, dur AS duration_ns
  ```
- **`|> SET`** replaces a column's values:
  ```sql
  FROM tree
  |> TREE ACCUMULATE UP SUM(self) AS total
  |> SET self = total
  |> DROP total
  ```

`SELECT`, `EXTEND` and `SET` only take columns today: `a + 1` is not supported
yet.

---

## Next Steps

- [PerfettoSQL Pipe Syntax Reference](/docs/analysis/perfetto-sql-pipe-syntax.md)
- [Migrating to PerfettoSQL Pipe Syntax](/docs/analysis/perfetto-sql-pipe-migration.md)
- [PerfettoSQL Pipe Operator Implementation Notes](/docs/design-docs/perfetto-sql-pipelines.md)
