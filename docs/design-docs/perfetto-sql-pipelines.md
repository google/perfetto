# PerfettoSQL Pipe Operator Implementation Notes

This page describes how pipelines are compiled and run, and how each stage is
implemented.

> **Warning:** Pipe syntax and its implementation are **experimental and
> unstable**. Do **not** rely on anything described here. Both the syntax and
> the execution behavior can and will change without notice.

Related pages:

- [PerfettoSQL Pipe Syntax Reference](/docs/analysis/perfetto-sql-pipe-syntax.md)
- [Migrating to PerfettoSQL Pipe Syntax](/docs/analysis/perfetto-sql-pipe-migration.md)
- [Getting Started with PerfettoSQL Pipe Syntax](/docs/analysis/perfetto-sql-pipe-getting-started.md)

---

## From Query to Rows

Code: `/src/trace_processor/perfetto_sql/pipeline/`

1. **Compile.** `Compiler` turns the parsed pipeline into a `LogicalPlan`: a
   tree of `PlanNode`s, one per source or stage, each holding a
   `PlanOperation`. Names are resolved and types checked here, so errors point
   at the token at fault.
2. **Prune.** `PruneColumns` drops what nothing reads (see
   [Column pruning](#column-pruning)).
3. **Serialize.** `SelectPipelineSql` writes the plan to bytes and wraps it in
   a SQL statement calling the `__intrinsic_pipeline` table function. Each SQL
   source becomes an argument built by `__intrinsic_dataframe_agg` (see
   [`FROM`](#from)).
4. **Lower.** When SQLite runs that statement, the plan is read back
   (`DeserializePlan`), the SQL sources are bound as dataframes
   (`BindDataframeArgs`), and `Lower` turns it into a `PhysicalPlan` of
   executor operators. A table read directly is checked to be the one the plan
   was compiled against.
5. **Run.** A `RowCursor` pulls batches through the operators and SQLite reads
   them row by row.

Each `PlanOperation` lowers itself through `Lowering`. Operators state what
they need, such as a column of a given type (`RequireType`), rows grouped by
some keys and sorted by `ts` (`RequireIntervalLanes`), or a tree order
(`RequireTreeWalk`). `Lowering` adds a conversion, sort or grouping only when
the rows are not already that way. Operators also declare what they produce,
such as sorted columns, so later stages can skip work.

An operator with more than one input, such as `INTERVAL INTERSECTION OF`, reads
each extra input through a side pipeline (`AddSidePipeline`) lowered the same
way.

### Batches

Operators pass columnar batches (`RowBatch`) of up to 2048 rows. Each column is
a view over a flat array, with an optional bitvector for `NULL`s. A batch has
one `Selection`: increasing row indices saying which rows are live. It can only
drop rows, so filtering copies nothing. Reordering rows, as sorting does,
copies them into new buffers (`RowStore::Gather`).

### Column pruning

Before lowering, `PruneColumns` walks from the pipeline's output back to its
sources and marks the columns that are read:

- Sources only read live columns. A scan always reads at least one column, so
  it still knows how many rows there are.
- `INTERVAL INTERSECTION OF` only keeps the operand columns that are read.
- Aggregates nobody reads are dropped from `TREE ACCUMULATE`, `INTERVAL
  FLATTEN` and `AGGREGATE`. If no `TREE ACCUMULATE` output is read, the whole
  stage goes.
- `INTERVAL FILL GAPS` drops outputs nobody reads.
- `WHERE`, `ORDER BY` and `DISTINCT` keep the columns they compare, and
  `AGGREGATE` keeps its keys.

---

## `FROM` {#from}

Code: `operations/scan.cc`, `/src/trace_processor/core/exec/dataframe_scan.cc`

Every source is read as a dataframe by `DataframeScan`.

### Tables

When `FROM` names a finalized dataframe, such as `slice`, `sched` or any
`PERFETTO TABLE`, the pipeline reads its storage directly:

- Only live columns are read, and hidden columns are skipped.
- Column buffers are referenced, not copied.
- Sorted columns are declared as such, so an `ORDER BY` on them is free.
- An `id` column stored as `0, 1, 2, ...` lets `TREE ACCUMULATE` skip its hash
  table.

### Views and subqueries

A view, virtual table or `SELECT` subquery has its column names and types
worked out by `RelationAnalyzer` when the pipeline compiles. If it only picks
and renames columns of a finalized table, the table is read directly as above.

Otherwise SQLite runs it once, and `__intrinsic_dataframe_agg` collects the
rows into a dataframe which is passed to the pipeline:

```sql
SELECT __intrinsic_dataframe_agg('<col_0>,<col_2>', "<col_0>", "<col_2>")
FROM <source>
```

Only live columns are listed, so SQLite can skip computing the rest.

---

## `INTERVAL INTERSECTION OF`

Code: `/src/trace_processor/core/exec/interval_intersect.cc`

`INTERVAL INTERSECTION OF` reads all its operands before it outputs anything.

### 1. Reading operands

Each operand is a side pipeline:

- `ts` and `dur` are converted to 64-bit integers. `PER` columns are converted
  to one type across operands; strings are allowed.
- Rows are grouped by the `PER` columns and sorted by `ts` within each group,
  if they are not already.
- Rows with a `NULL` `ts` or `dur` are skipped. A negative bound, or a
  `ts + dur` that overflows, is an error.
- Only operand columns read downstream are kept.

All operands number their `PER` keys in one shared `GroupTable`, so a key has
the same number everywhere. A key that no earlier operand has can produce no
region, so its rows are dropped as they are read. While a lane is read, the
operator notes whether any two of its intervals overlap.

### 2. Intersecting each key

Only keys of the operand with the fewest keys can start a region, so the
operator walks those and skips any key another operand lacks. For each key:

- **No overlaps:** If no operand's intervals overlap each other, one sweep
  moves through all operands at once and writes regions straight to the
  output.
- **Overlaps:** Otherwise the operands narrow the regions one at a time,
  smallest first. Each step uses `IntervalIntersector`, which picks a linear
  sweep, binary search or interval tree depending on the operand and how many
  regions are left.

Regions are stored as flat arrays of bounds and operand rows, so no region
needs its own allocation.

### 3. Output

Regions are output in batches of up to 2048. Each batch holds the region's `ts`
and `dur`, then the kept columns of each operand, copied from the rows the
region came from.

### Names

The compiler puts the region's `ts` and `dur` first in the row, then every
operand column. Each operand column is marked `qualified_only` when only its
alias should find it: the operand's own `ts` and `dur`, and the `PER` columns of
operands after the first. Everything else is found by its bare name too.

---

## `INTERVAL FLATTEN`

Code: `/src/trace_processor/core/exec/interval_flatten.cc`

Input is grouped by the `PER` keys and sorted by `ts` within each group,
through `RequireIntervalLanes`, which adds a sort only if needed.
`IntervalFlatten` then streams through the sorted rows.

A min-heap tracks when active intervals end. Each interval keeps its row's
aggregate states in a slot, and a batch of segments is aggregated together from
the slots live in each, so a segment does not rescan every active interval.
While no input has been `NULL`, aggregates that can remove a value, as counts
and sums can, add each slot at its interval's first segment, remove it at its
end and keep running totals. Otherwise, as for `MIN` and `MAX`, each slot is
merged into a tree over the batch's segments. A sum notes whether it added any
value, so a sum of only `NULL`s stays `NULL`.

Points at one instant are collected and output together once every row
starting there has been read. Their totals include intervals active at that
instant but do not carry into the next segment.

Segments are output in bounded batches; only active intervals are held.
Endpoint and sum arithmetic is checked for overflow.

Unused aggregates are pruned, but the stage stays even with none left, since it
still decides the rows.

---

## `INTERVAL FILL GAPS`

Code: `/src/trace_processor/core/exec/interval_fill_gaps.cc`

The background is a side pipeline, read whole before the input. Both are
grouped by the `PER` keys and sorted by `ts` through `RequireIntervalLanes`,
and columns shared by the two are converted to one type.

`IntervalFillGaps` streams through the input, outputs every row unchanged, and
adds a filler for each part of the background that no input row in the same
lane covers. It runs in one of two ways:

- **The background has the `PER` columns, or there are none.** Each background
  row belongs to one lane. A lane with no input is filled whole.
- **The background has no `PER` columns but the input does.** Every input lane
  is filled against the whole background.

A filler takes each column from the background column of the same name, else
the lane's `PER` value, else `NULL`. An input `dur` of `-1` lasts forever.
Fillers never have zero duration, and output rows are in no particular order.

---

## `TREE ACCUMULATE`

Code:
- `/src/trace_processor/core/exec/tree_number_nodes.cc`
- `/src/trace_processor/core/exec/tree_order.cc`
- `/src/trace_processor/core/exec/tree_accumulate.cc`

A `TREE ACCUMULATE` stage lowers to three steps: numbering nodes, ordering
rows, and accumulating.

### 1. Numbering nodes: `TreeNumberNodes`

The tree operators keep state per node. An `id` can be any 64-bit integer or a
string, so arrays indexed by it could be huge or impossible.

`TreeNumberNodes` adds two dense `Uint32` columns to each batch: the row's node
number, given out `0, 1, 2, ...` in order of first sighting, and its parent's,
or `kNoNode` for a root. Later operators only read these, so per-node arrays
are as long as the input.

It has three paths:

1. **`Id` columns:** When reading an unfiltered dataframe whose `id` is stored
   as `Id`, whose `parent_id` is `Uint32`, and where every parent comes before
   its children, the numbers are copied straight from the columns.
2. **Dense integers:** While the `id`s seen are `0, 1, 2, ...` and every parent
   has been seen, each `id` is its own number and nothing is hashed.
3. **Hash map:** From the first `id` that breaks this, or any string `id`, a
   `FlatHashMap` maps ids to numbers. Ids seen before are added to it first.

### 2. Ordering rows: `TreeChildFirst` and `TreeParentFirst`

Rows must arrive so that what a node depends on is done before it. This needs
no strict depth-first order: subtrees can interleave, since the accumulator
keeps a total per node.

#### `UP`: `TreeChildFirst`

`UP` needs every child before its parent. A row's children may come at the very
end, so `TreeChildFirst` reads all rows before it outputs any:

- It notes whether the input is already child first, or already parent first.
- **Child first:** outputs the rows as they came.
- **Parent first:** outputs them in reverse.
- **Neither:** builds a child list over the node numbers, walks it from the
  roots to get a parent-first order, checks that every row was reached, which
  catches cycles, and reverses it.

#### `DOWN`: `TreeParentFirst`

`DOWN` needs every parent before its children. A row can go as soon as its
parent has, so `TreeParentFirst` streams:

- A bitvector marks the nodes already output.
- A root, or a row whose parent is out, passes straight through. A batch where
  every row passes is forwarded without copying.
- A row that arrives before its parent is copied aside onto its parent's
  waiting list. When the parent is output, its waiting children and their
  waiting descendants follow.
- Only rows that arrive before their parents cost memory or copies.

### 3. Accumulating: `TreeAccumulateUp` and `TreeAccumulateDown`

One streaming operator computes every aggregate of the stage, using the
executor's shared aggregate functions
(`/src/trace_processor/core/exec/aggregate_function.h`), as `INTERVAL FLATTEN`
and `AGGREGATE` do:

- Aggregated columns are converted to flat `Int64`. A sum skips `NULL`s and
  notes whether it added anything, so a sum of nothing is `NULL`. Overflow is
  an error.
- Every node's aggregate states sit in one vector indexed by node number. For
  each batch, every row is added into its node's state, then states are merged
  along the batch's edges in row order:
  - **Up:** Children come first, so when a node arrives its descendants are
    already merged into it. It is then merged into its parent.
  - **Down:** Parents come first, so when a node arrives its parent already
    covers every ancestor. The parent is merged into it.

### 4. Several `TREE ACCUMULATE` stages

- Later stages reuse the first stage's node numbers while `id` and `parent_id`
  are the same columns.
- Two `UP` stages in a row, or two `DOWN` stages, reuse the first stage's row
  order.
- Switching between `UP` and `DOWN` keeps the numbers and reorders the rows.

---

## `GRAPH ... TREE`

Code:
- `/src/trace_processor/core/exec/graph_tree.cc`
- `/src/trace_processor/util/graph.cc`

`NODES`, `EDGES` and `FROM` are each a side pipeline, with their ids converted
to `Int64`. `GraphTree` reads all three before it outputs anything:

- Node rows are kept in a `RowStore`, and each node is numbered by its row.
  Ids close to dense are found through an array, and any others through a
  `FlatHashMap`.
- Edges are grouped by source, keeping the order they arrived in.
- The search or dominator tree runs over these numbers, from the seeds in the
  order they arrived. A dominator tree grows from one extra node with an edge
  to every seed, which is dropped from the output.

The order of the seeds and edges picks between equally good parents, so the
lowering adds nothing which reorders those inputs.

Rows go out parents first, gathering each node's row, with its parent's `id`
and the two `Uint32` columns `TreeNumberNodes` would add. The lowering records
these as a `DOWN` walk of `id` and `parent_id`, so a `TREE ACCUMULATE DOWN`
after it needs no other operator, and an `UP` one only reverses the rows.

---

## `AGGREGATE`

Code: `/src/trace_processor/core/exec/hash_aggregate.cc`

`AGGREGATE` lowers to one `HashAggregate`, which reads all input before
outputting. `GroupTable` hashes each row's keys to find its group, numbering
groups in the order they are first seen. Integer keys of any width compare
equal by value, strings by their interned id, and `NULL`s equal each other.
Each group's aggregate states sit beside its keys, and every row is added as it
is read, so memory grows with the number of groups, not rows. At the end,
groups are output in the order they were first seen. If the input was sorted by
the keys, the output is declared sorted too.

Without `GROUP BY` there is one group, which is output even when there were no
rows.

Unused aggregates are pruned. The stage itself always stays, since it decides
the rows.

The compiler replaces the row with the keys and then the aggregates, and clears
all aliases.

---

## `WHERE`

Code: `operations/where.cc`, `/src/trace_processor/core/exec/filter.cc`

`WHERE` lowers to one streaming `Filter`. Each comparison's literal, or `IN`
list, is cast to its column's type once, during lowering, and stored with the
plan (`PhysicalPlan::filter_values()`). A string is cast to its id in the
string pool, so `=` and `!=` compare ids, and a string no row holds matches
nothing. Casting during lowering is safe because a plan is lowered for each
cursor, after its inputs are bound, so every string they hold is in the pool. A
column with no known type is first converted to the literals' type, a string
if any literal is one. `IS [NOT] NULL` reads any column, including one carrying
a type per row. `BETWEEN` is two comparisons. `Filter` compares with the same
kernels as the dataframe's own filters and narrows each batch's selection, so
nothing is copied.

---

## `DISTINCT`

Code: `operations/distinct.cc`, `/src/trace_processor/core/exec/distinct.cc`

`DISTINCT` lowers to one streaming `Distinct`. A `GroupTable` holds the keys of
the rows kept so far; a row whose keys are already there is dropped by
narrowing the selection. Memory grows with the number of distinct rows, and
rows keep their order. A row can have at most 64 columns.

---

## `ORDER BY`

Code: `/src/trace_processor/core/exec/sort.cc`

`Sort` reads every row before outputting any. It encodes each row's keys into
one fixed-width record that compares as bytes, sorts the row indices by those
records, then copies the rows out in that order.

A string key is encoded as its rank among the strings of the rows, so it is
compared by contents, not by its id in the string pool. Ranks are only known
once every row is in, so string keys are encoded just before sorting. A key
from SQLite whose type is not known is converted to integers.

Lowering tracks the order rows are in. A single ascending key that a table
already keeps sorted, such as `ts` on `slice`, needs no `Sort`. A `Sort` that
nothing reads the order of, because a later stage sorts, groups or walks a tree
first, is removed. After a sort, the tree order is forgotten, so a later
`TREE ACCUMULATE` reorders the rows but keeps its node numbers.

---

## `LIMIT`

Code: `operations/limit.cc`, `/src/trace_processor/core/exec/limit.cc`

`LIMIT` lowers to one streaming `Limit`, which narrows each batch's selection.
It closes any open `Sort` before it, so `ORDER BY` is never removed from in
front of a `LIMIT`. Once it has kept `count` rows it takes no more input, so
nothing before it reads further than the rows it needs. Rows kept stay in their
order, so every known arrangement survives.

---

## `SELECT`, `EXTEND`, `DROP`, `RENAME`, `SET` and `AS`

Code: `/src/trace_processor/perfetto_sql/pipeline/operations/projection.cc`

These stages only exist while the pipeline compiles. They add no operators
and copy no data.

The compiler keeps the current row as a list of `RowColumn`s: a name, the
`ColumnId` holding its values, and whether it is `qualified_only`, meaning only
an alias finds it. Next to it is a list of aliases, each a copy of the row from
when it was set.

- `DROP` removes the columns of that name a bare name finds, and any alias of
  that name.
- `RENAME` changes the name of a column.
- `SET` points a column at another `ColumnId`, and removes any alias of that
  name.
- `EXTEND` adds columns at the end. A bare `*` is refused.
- `AS` drops `qualified_only` columns from the row, and replaces every alias
  with one for the row that is left.
- `SELECT` replaces the row with the listed columns and clears every alias. A
  bare `*` only takes columns that are not `qualified_only`. Every column it
  lists, including those from `<alias>.*`, can then be found by bare name.

`AGGREGATE`, `INTERVAL FLATTEN` and `INTERVAL FILL GAPS` also rebuild the row
and aliases.

When compiling ends, the columns of the row that are not `qualified_only`
become the pipeline's output. Pruning starts from those, and lowering maps each
to a column of the final batch. Reordering, duplicating, renaming or dropping
columns never copies data.
