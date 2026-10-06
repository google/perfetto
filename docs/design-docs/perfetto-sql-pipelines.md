# PerfettoSQL Pipe Operator Implementation Notes

This page describes how each PerfettoSQL pipeline source and operator is
implemented, what data structures and fast paths it uses, and what its memory
and streaming characteristics are.

> **Warning:** Pipe syntax and its implementation are **experimental and
> unstable**. Do **not** rely on anything described here. Both the syntax and
> the execution behavior can and will change without notice.

Related pages:

- [PerfettoSQL Pipe Syntax Reference](/docs/analysis/perfetto-sql-pipe-syntax.md)
- [Migrating to PerfettoSQL Pipe Syntax](/docs/analysis/perfetto-sql-pipe-migration.md)
- [Getting Started with PerfettoSQL Pipe Syntax](/docs/analysis/perfetto-sql-pipe-getting-started.md)

---

## Overview: Batches and Pruning

Pipelines execute over columnar batches of up to 8192 rows. Each column in a
batch is a view over a contiguous array, an optional validity bitvector for
nulls, and an optional row-selection index buffer for filtering or reordering
rows without copying values.

Before any operator runs, a backward liveness pass walks from the pipeline's
final output columns back to the sources:

- Unused columns are dropped from table and subquery scans.
- Unused columns carried through `INTERVAL INTERSECTION OF` are dropped before
  the operands are read into memory.
- Unused `SUM(...)` aggregates in `TREE ACCUMULATE` are dropped. If none of a
  `TREE ACCUMULATE` stage's output columns are read downstream, the entire stage
  is removed from the plan, skipping node numbering, tree ordering, and
  accumulation.

---

## `FROM`

How a `FROM` source is read depends on what it names.

### Finalized `PERFETTO TABLE`s and built-in tables

When `FROM` names an unqualified table backed by a finalized Trace Processor
dataframe, such as `slice`, `sched`, or any table created with
`CREATE PERFETTO TABLE`, the pipeline reads the dataframe's columnar storage
directly in C++ without opening a SQLite cursor:

- Only the columns needed by downstream stages are read.
- String and integer buffers are referenced directly in each batch rather than
  copied per row.
- If a table has an `id` column stored as an implicit `0, 1, 2, ...` row-index
  sequence, downstream operators like `TREE ACCUMULATE` detect this storage type
  and skip hash-table lookups.

### Views and `SELECT` subqueries

When `FROM` names a view, a virtual table, or a parenthesized `SELECT` subquery,
Trace Processor prepares and steps the SQL query in SQLite and copies the
returned values into columnar batches.

If column pruning finds that some columns of the SQL source are never read
downstream, the source query is wrapped before preparing it in SQLite:

```sql
WITH __pipeline_source(c0, c1, c2, ...) AS (SELECT * FROM <source>)
SELECT c0 AS "<col_0>", c2 AS "<col_2>" FROM __pipeline_source
```

Projecting only the needed positional columns lets SQLite's query planner drop
unused expressions or joins when it flattens the subquery, and avoids copying
unused columns out of SQLite.

---

## `INTERVAL INTERSECTION OF`

Implementation: `/src/trace_processor/core/exec/interval_intersect.cc`

`INTERVAL INTERSECTION OF` is a blocking source: it reads all of its operands in
full before emitting the first output batch.

### 1. Reading and partitioning operands

Each operand is executed as an independent sub-pipeline and read into memory:

- `ts`, `dur`, and all `PER` columns must be 64-bit integers. Rows where `ts` or
  `dur` is `NULL` are skipped. Rows where `ts < 0` or `dur < 0` stop execution
  with an error.
- Only the operand columns that are referenced downstream are buffered in the
  operand's `RowStore`. Unreferenced columns are discarded as batches arrive.
- Each valid row's `[ts, ts + dur)` interval and row index are inserted into a
  per-operand hash map keyed by the `PER` columns. Each `PER` column contributes
  1 presence byte and 8 value bytes to the partition key, so two rows with
  `NULL` in a `PER` column land in the same partition.
- Once an operand finishes reading, the intervals in each partition are sorted
  by start, end, and row index, then scanned once to record whether any two
  intervals in that partition overlap.

### 2. Intersecting within each partition

A partition can only produce output if its `PER` key exists in every operand.
The operator iterates over the keys of whichever operand has the fewest distinct
`PER` keys, and skips any key missing from another operand.

For each key present in all operands, it chooses between two paths:

- **All-disjoint fast path:** If every operand's intervals in that partition are
  non-overlapping, it runs a single multi-way linear sweep across all operands
  at once, emitting regions directly to the output buffer with no intermediate
  region allocations.
- **Pairwise narrowing path:** If at least one operand has overlapping intervals
  in that partition, the operands for that key are sorted in ascending order of
  interval count. The smallest operand seeds the running set of candidate
  regions. Each remaining operand narrows the running set one at a time using
  `IntervalIntersector`, which picks between a linear sweep, binary search, or
  an interval tree based on whether that operand has overlaps and how many
  candidate regions are currently active.

Candidate and output regions are stored as two flat arrays of bounds and one
flat array of operand row indices, avoiding per-region heap allocations.

### 3. Serving output batches

Once all partitions are intersected, output is served in batches of up to 8192
regions. Each batch writes the region `ts` and `dur` into two flat `Int64`
columns and attaches row-selection views into each operand's buffered `RowStore`
for the retained columns.

---

## `INTERVAL FLATTEN`

Implementation: `/src/trace_processor/core/exec/interval_flatten.cc`

The planner prepares input grouped by the `PER` keys and ordered by `ts`
within each group, adding sorting and grouping only when the existing order
cannot be reused. These preparation steps can buffer the input; the flattening
step itself processes ordered batches incrementally.

`IntervalFlatten` sweeps one group at a time. A min-heap tracks active interval
endpoints. Running counts and sums are updated when intervals start or expire,
so producing a segment does not require scanning every active interval. Each
active interval retains its contributions so they can be subtracted at its
endpoint. Sums also track the number of non-NULL contributions, allowing an
all-NULL sum to remain NULL.

Points at a timestamp are accumulated separately and emitted together once all
rows starting at that timestamp have been consumed. Their totals include
positive-duration intervals active at that instant, but are not carried into
the next positive-duration segment.

Completed segments are emitted in bounded batches. The sweep retains active
interval state and the grouping keys needed for output, rather than buffering
all completed segments. Finalization drains intervals still active when the
input ends. Endpoint and sum arithmetic is checked for integer overflow.

Unused aggregates are pruned, but the stage remains even if no aggregates are
needed: flattening still changes the rows and their interval boundaries.

---

## `TREE ACCUMULATE`

Implementation:
- `/src/trace_processor/core/exec/tree_number_nodes.cc`
- `/src/trace_processor/core/exec/tree_order.cc`
- `/src/trace_processor/core/exec/tree_accumulate.cc`

A `TREE ACCUMULATE` stage lowers into three steps: node numbering, tree
ordering, and accumulation.

### 1. Node numbering: `TreeNumberNodes`

Tree operators need per-node state: visited bitvectors, waiting lists, and
running sums. Raw `id` values from a filtered query can be arbitrary 64-bit
integers or strings scattered across a wide range, so indexing an array directly
by `id` would either fail or allocate memory proportional to the largest `id` in
the original table.

`TreeNumberNodes` solves this by appending two dense `Uint32` columns to each
batch: the row's node number, assigned `0, 1, 2, ...` in order of first
sighting, and its parent's node number, using `kNoNode` for roots where
`parent_id IS NULL`. Downstream tree operators only inspect these two dense
`Uint32` columns, so all per-node arrays are sized to the number of rows in the
input.

`TreeNumberNodes` has three tiers of execution:

1. **Dataframe `Id` fast path:** When scanning an unfiltered dataframe whose
   `id` column has storage type `Id`, whose `parent_id` column is `Uint32`, and
   where every row's parent appears at an earlier row, node numbers are assigned
   in place by copying `parent_id` and filling `0, 1, 2, ...` with no hash map
   and no per-cell type dispatch.
2. **Dense integer fast path:** When `id` or `parent_id` arrives as a general
   integer column from a SQLite subquery, the operator tracks whether the IDs
   seen so far are still `0, 1, 2, ...` with parents referencing already-seen
   IDs. As long as that holds, each `id` is already its own node number and the
   hash map stays empty.
3. **Hash map path:** On the first non-dense integer or string `id`, the
   operator backfills its `FlatHashMap` with any previously seen dense IDs and
   maps all subsequent `id` and `parent_id` values through the hash map.

### 2. Tree ordering: `TreeChildFirst` and `TreeParentFirst`

Accumulating up or down a tree requires rows to be ordered so that the values a
node depends on have already been computed when the node is visited. Neither
direction requires strict depth-first traversal order. Subtrees may be
interleaved because the accumulator stores a running total per node rather than
a single root-to-leaf stack.

#### `UP`: `TreeChildFirst`

`TREE ACCUMULATE UP` requires every child to appear before its parent. Because a
streaming operator cannot know whether an arriving node has children until the
end of the input, `TreeChildFirst` is a blocking operator that reads all input
rows before emitting:

- As rows arrive, it tracks two flags: whether the input is already child-first,
  meaning no row's parent has been seen before the row itself, and whether it is
  parent-first, meaning every non-root row's parent has already been seen.
- **Already child-first:** Emits the buffered batches in arrival order without
  building an index permutation.
- **Already parent-first:** Emits the buffered rows in reverse arrival order,
  since reversing any parent-first ordering yields a valid child-first ordering.
- **Unordered:** Builds a flat compressed-sparse-row child list over the node
  numbers, traverses from the roots with an explicit stack to produce a
  parent-first order, verifies that all rows were reached to catch cycles, and
  reverses the result.

#### `DOWN`: `TreeParentFirst`

`TREE ACCUMULATE DOWN` requires every parent to appear before its children.
Unlike `UP`, this is not a full pipeline breaker: a row can be emitted as soon
as its parent has been emitted.

- `TreeParentFirst` keeps a bitvector of nodes that have already been emitted.
- When a batch arrives, any root row or row whose parent is already marked as
  emitted passes straight through. If every row in the batch passes, the batch
  is forwarded as-is without copying any columns.
- Only rows that arrive before their parent are copied into a side buffer and
  linked into a per-parent waiting list. The moment that parent row arrives and
  is emitted, its waiting children and any of their waiting descendants are
  released and emitted after the batch.
- Memory and copying cost are proportional to how many rows arrive before their
  parents. When the input is already parent-first, nothing is buffered or
  copied.

### 3. Accumulation: `TreeAccumulateUp` and `TreeAccumulateDown`

Each `SUM(col)` in the stage runs a streaming `TreeAccumulateUp` or
`TreeAccumulateDown` operator over the ordered batches:

- The summed column is verified or widened to flat `Int64`, with `NULL` values
  contributing `0`. Checked 64-bit addition is used so integer overflow is
  reported as an error rather than wrapping silently.
- The operator keeps a single `std::vector<int64_t>` indexed by node number:
  - **`TreeAccumulateUp`:** When a row for `node` arrives, every descendant has
    already been processed and added its subtree sum into `by_node[node]`. The
    operator sets `total = value + by_node[node]`, writes `total` to the output
    batch, and adds `total` into `by_node[parent]`.
  - **`TreeAccumulateDown`:** When a row for `node` arrives, its `parent` has
    already been processed and stored its path sum in `by_node[parent]`. The
    operator sets `total = value + by_node[parent]`, stores `by_node[node] =
    total`, and writes `total` to the output batch.

### 4. Reuse across consecutive `TREE ACCUMULATE` stages

When a pipeline chains multiple `TREE ACCUMULATE` stages:

- Consecutive `TREE ACCUMULATE` stages share the same `TreeNumberNodes` columns
  as long as `id` and `parent_id` still refer to the same underlying columns.
- Two consecutive `TREE ACCUMULATE UP` stages, or two consecutive `DOWN` stages,
  reuse the tree ordering from the first stage and skip inserting a second
  ordering operator.
- Switching between `UP` and `DOWN` reuses the node numbering and re-orders the
  rows for the new direction.

---

## `SELECT`, `EXTEND`, `DROP`, `RENAME`, `SET`, and `AS`

Implementation: `/src/trace_processor/perfetto_sql/pipeline/compiler.cc`

The column-reshaping stages `SELECT`, `EXTEND`, `DROP`, `RENAME`, `SET`, and
`AS` have no runtime operators and do no work during batch execution.

During compilation, the pipeline compiler maintains the current row as a vector
of `(column_name, ColumnId, qualified_only)` entries, alongside a list of active
table aliases. Each active alias holds a snapshot of that vector from when the
alias was bound:

- `DROP` removes entries from the compile-time row vector.
- `RENAME` changes the `column_name` string on matching entries.
- `SET` updates a target entry's `ColumnId` to point at the source column's
  `ColumnId`.
- `EXTEND` appends new `(column_name, ColumnId)` entries to the row vector.
- `AS` replaces the active alias list with a copy of the current row vector.
- `SELECT` replaces the row vector with the selected `(column_name, ColumnId)`
  entries and clears the active alias list.

At the end of compilation, only the `ColumnId`s present in the final row vector
are marked live for column pruning and mapped to physical batch column indices
when returning results to SQLite. Reordering, duplicating, renaming, or dropping
columns in pipe stages never copies column data at runtime.
