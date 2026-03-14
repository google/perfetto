# Trees in Perfetto

**Authors:** @LalitMaganti

**Status:** Prototyping

**PR:** N/A

## Problem

- Working with trees in bare SQL is painful
  - Recursive SQL is not a solution. It's highly inefficient, completely
    unintuitive and straight up does not allow aggregation per node.
- We have some basic tree/graph operators but they don't work for several
  reasons:
  - Require constantly joining with id to recover initial columns, don't
    "passthrough" columns
  - Inefficient because they constantly convert back to flat tables after every
    step so if you want to do a bunch of operations one after another, you end
    up converting back/forth a lot, wasting a _ton_ of performance
  - Unintuitive: constantly swithcing between tree and tabluar representation
    introduces tremendous cognitive load making it impossible to reason about
    anything beyond most basic things: case in point flamegraph.sql
- Lots of places in Perfetto where we care about trees:
  - slice stacks - both in ancestor/descendant and when connected by flows
  - heap dumps (graphs which are normalized to trees)
  - heap profiles
  - stack samples

## Examples

Before diving into the design, here are concrete examples showing what the tree
SQL syntax looks like in practice.

### Basic concepts

Trees are an opaque type in SQL. You convert a table into a tree, perform
operations on it, and convert back to a table when you need results.

**Conversion functions:**
- `tree_from_table!((<query>), (<columns>))`: converts a table with `id` and
  `parent_id` columns into a tree, carrying the listed columns along.
- `tree_to_table!(<tree>, (<columns>))`: converts a tree back to a table with
  `tree_id` (dense 0-based index) and `tree_parent_id` columns, plus the
  original columns.

**Tree operations** (all take a tree and return a new tree):
- `tree_filter`: remove nodes, reparenting surviving children.
- `tree_propagate_down`: compute cumulative values from root to leaves.
- `tree_propagate_up`: compute cumulative values from leaves to root.
- `tree_merge_siblings`: merge sibling nodes with the same group key.
- `tree_merge_siblings_ordered`: like above, but only merges consecutive runs.
- `tree_merge_into_parent`: merge a node into its parent if they share a group.
- `tree_invert`: flip a tree so leaves become roots, merging by group key.

All operations compose: the output of one is a valid input to the next.

### Filter

Removes nodes matching a condition. Surviving children of removed nodes are
reparented to the nearest surviving ancestor (or become roots).

```sql
CREATE PERFETTO TABLE calls AS
SELECT 1 AS id, NULL AS parent_id, 'main' AS fn, 100 AS dur
UNION ALL SELECT 2, 1, 'parse', 60
UNION ALL SELECT 3, 2, 'lex', 30     -- child of parse
UNION ALL SELECT 4, 1, 'alloc', 5    -- short, will be filtered
UNION ALL SELECT 5, 1, 'emit', 35;

-- Remove short calls. Node 4 (alloc, dur=5) is removed.
SELECT tree_id, tree_parent_id, fn, dur
FROM tree_to_table!(
  tree_filter(
    tree_from_table!((SELECT * FROM calls), (fn, dur)),
    tree_where(tree_constraint('dur', '>=', 10))
  ),
  (fn, dur)
)
ORDER BY id;
```

| tree_id | tree_parent_id | fn    | dur |
|---------|----------------|-------|-----|
| 0       | NULL           | main  | 100 |
| 1       | 0              | parse | 60  |
| 2       | 1              | lex   | 30  |
| 3       | 0              | emit  | 35  |

When an intermediate node is removed, its children are reparented:

```sql
CREATE PERFETTO TABLE calls AS
SELECT 1 AS id, NULL AS parent_id, 'main' AS fn
UNION ALL SELECT 2, 1, 'wrapper'   -- will be filtered
UNION ALL SELECT 3, 2, 'real_work' -- reparented to main
UNION ALL SELECT 4, 2, 'cleanup';  -- reparented to main

SELECT tree_id, tree_parent_id, fn
FROM tree_to_table!(
  tree_filter(
    tree_from_table!((SELECT * FROM calls), (fn)),
    tree_where(tree_constraint('fn', '!=', 'wrapper'))
  ),
  (fn)
)
ORDER BY id;
```

| tree_id | tree_parent_id | fn        |
|---------|----------------|-----------|
| 0       | NULL           | main      |
| 1       | 0              | real_work |
| 2       | 0              | cleanup   |

Multiple constraints are ANDed. Filters can also be chained by nesting
`tree_filter()` calls.

### Propagate down

Computes cumulative values from root toward leaves. For each node:
`column[child] = f(column[parent], column[child])`.

```sql
-- Compute depth: initialize every node with 1, then SUM downward.
-- Root gets 1, its children get 1+1=2, etc.
CREATE PERFETTO TABLE calls AS
SELECT 1 AS id, NULL AS parent_id, 'main' AS fn, 1 AS ones
UNION ALL SELECT 2, 1, 'parse', 1
UNION ALL SELECT 3, 2, 'lex', 1
UNION ALL SELECT 4, 1, 'emit', 1
UNION ALL SELECT 5, 4, 'write', 1;

SELECT tree_id, fn, depth
FROM tree_to_table!(
  tree_propagate_down(
    tree_from_table!((SELECT * FROM calls), (fn, ones)),
    'ones', 'SUM', 'depth'
  ),
  (fn, depth)
)
ORDER BY id;
```

| tree_id | fn    | depth |
|---------|-------|-------|
| 0       | main  | 1     |
| 1       | parse | 2     |
| 2       | lex   | 3     |
| 3       | emit  | 2     |
| 4       | write | 3     |

The 4th argument names the output column. Supported aggregate functions:
`SUM`, `MIN`, `MAX`, `FIRST`, `LAST`.

### Propagate up

Computes cumulative values from leaves toward root. Multiple children's values
are reduced into a single value before combining with the parent.

```sql
-- Compute cumulative duration: each node gets the sum of its own dur
-- plus all descendants' dur.
CREATE PERFETTO TABLE calls AS
SELECT 1 AS id, NULL AS parent_id, 'main' AS fn, 10 AS self_dur
UNION ALL SELECT 2, 1, 'parse', 30
UNION ALL SELECT 3, 2, 'lex', 20
UNION ALL SELECT 4, 1, 'emit', 15;

SELECT tree_id, fn, self_dur, cumulative_dur
FROM tree_to_table!(
  tree_propagate_up(
    tree_from_table!((SELECT * FROM calls), (fn, self_dur)),
    'self_dur', 'SUM', 'cumulative_dur'
  ),
  (fn, self_dur, cumulative_dur)
)
ORDER BY id;
```

| tree_id | fn    | self_dur | cumulative_dur |
|---------|-------|----------|----------------|
| 0       | main  | 10       | 75             |
| 1       | parse | 30       | 50             |
| 2       | lex   | 20       | 20             |
| 3       | emit  | 15       | 15             |

### Merge siblings (unordered)

Merges all sibling nodes that share the same group key, regardless of their
position among siblings. Requires specifying how to aggregate remaining columns.

```sql
-- Two "alloc" calls under main should be merged into one.
CREATE PERFETTO TABLE calls AS
SELECT 1 AS id, NULL AS parent_id, 'main' AS fn, 100 AS dur
UNION ALL SELECT 2, 1, 'alloc', 20
UNION ALL SELECT 3, 1, 'parse', 60
UNION ALL SELECT 4, 1, 'alloc', 15;

SELECT tree_id, tree_parent_id, fn, dur
FROM tree_to_table!(
  tree_merge_siblings(
    tree_from_table!((SELECT * FROM calls), (fn, dur)),
    tree_group('fn'),
    tree_agg('dur', 'SUM')
  ),
  (fn, dur)
)
ORDER BY tree_id;
```

| tree_id | tree_parent_id | fn    | dur |
|---------|----------------|-------|-----|
| 0       | NULL           | main  | 100 |
| 1       | 0              | alloc | 35  |
| 2       | 0              | parse | 60  |

### Merge siblings (ordered)

Like unordered merge, but only merges siblings that are consecutive in the same
group. When the group key changes, a new merged node begins.

```sql
-- A-B-A pattern: the two A's are NOT merged because B separates them.
CREATE PERFETTO TABLE calls AS
SELECT 1 AS id, NULL AS parent_id, 'main' AS fn, 100 AS dur
UNION ALL SELECT 2, 1, 'A', 10
UNION ALL SELECT 3, 1, 'A', 15
UNION ALL SELECT 4, 1, 'B', 20
UNION ALL SELECT 5, 1, 'A', 12;

SELECT tree_id, tree_parent_id, fn, dur
FROM tree_to_table!(
  tree_merge_siblings_ordered(
    tree_from_table!((SELECT * FROM calls), (fn, dur)),
    tree_group('fn'),
    tree_agg('dur', 'SUM')
  ),
  (fn, dur)
)
ORDER BY tree_id;
```

| tree_id | tree_parent_id | fn   | dur |
|---------|----------------|------|-----|
| 0       | NULL           | main | 100 |
| 1       | 0              | A    | 25  |
| 2       | 0              | B    | 20  |
| 3       | 0              | A    | 12  |

Nodes 2 and 3 (both "A", consecutive) merged into dur=25. Node 5 ("A") stays
separate because node 4 ("B") breaks the run.

### Merge into parent

If a node shares the same group key as its parent, it is merged into the parent.

```sql
-- Recursive mutex: nested lock calls should collapse into the outermost one.
CREATE PERFETTO TABLE calls AS
SELECT 1 AS id, NULL AS parent_id, 'main' AS fn, 100 AS dur
UNION ALL SELECT 2, 1, 'lock', 80
UNION ALL SELECT 3, 2, 'lock', 60    -- same as parent, merge up
UNION ALL SELECT 4, 3, 'work', 40
UNION ALL SELECT 5, 3, 'lock', 20;   -- same as grandparent, merge up

SELECT tree_id, tree_parent_id, fn, dur
FROM tree_to_table!(
  tree_merge_into_parent(
    tree_from_table!((SELECT * FROM calls), (fn, dur)),
    tree_group('fn'),
    tree_agg('dur', 'MAX')
  ),
  (fn, dur)
)
ORDER BY tree_id;
```

| tree_id | tree_parent_id | fn   | dur |
|---------|----------------|------|-----|
| 0       | NULL           | main | 100 |
| 1       | 0              | lock | 80  |
| 2       | 1              | work | 40  |

Nodes 3 and 5 ("lock") merged into node 2 ("lock") because they share the
group key with their ancestor. Node 4 ("work") is reparented to the surviving
"lock" node.

### Invert tree

Flips a tree so leaves become roots and roots become leaves, merging nodes by
group key as the structure is reversed. Useful for converting "top-down" call
stacks into "bottom-up" views.

```sql
-- Top-down call stack:
--   main -> parse -> lex
--   main -> emit  -> lex
-- Inverted (bottom-up), the two "lex" leaves become a single root
-- with two children (parse, emit), each parented under "main".
CREATE PERFETTO TABLE calls AS
SELECT 1 AS id, NULL AS parent_id, 'main' AS fn, 1 AS cnt
UNION ALL SELECT 2, 1, 'parse', 1
UNION ALL SELECT 3, 2, 'lex', 1
UNION ALL SELECT 4, 1, 'emit', 1
UNION ALL SELECT 5, 4, 'lex', 1;

SELECT tree_id, tree_parent_id, fn, cnt
FROM tree_to_table!(
  tree_invert(
    tree_from_table!((SELECT * FROM calls), (fn, cnt)),
    tree_group('fn'),
    tree_agg('cnt', 'SUM')
  ),
  (fn, cnt)
)
ORDER BY tree_id;
```

| tree_id | tree_parent_id | fn    | cnt |
|---------|----------------|-------|-----|
| 0       | NULL           | lex   | 2   |
| 1       | 0              | parse | 1   |
| 2       | 1              | main  | 1   |
| 3       | 0              | emit  | 1   |
| 4       | 3              | main  | 1   |

### Composing operations

Operations compose naturally by nesting. Here's a realistic example: building a
simplified flamegraph from a call stack.

```sql
CREATE PERFETTO TABLE calls AS
SELECT 1 AS id, NULL AS parent_id, 'main' AS fn, 100 AS dur, 1 AS ones
UNION ALL SELECT 2, 1, 'parse', 60, 1
UNION ALL SELECT 3, 2, 'alloc', 20, 1
UNION ALL SELECT 4, 2, 'lex', 30, 1
UNION ALL SELECT 5, 1, 'alloc', 5, 1     -- too short, noise
UNION ALL SELECT 6, 1, 'emit', 35, 1
UNION ALL SELECT 7, 6, 'alloc', 15, 1
UNION ALL SELECT 8, 6, 'write', 15, 1;

-- Pipeline:
-- 1. Filter out short calls (dur < 10)
-- 2. Merge sibling "alloc" calls under each parent
-- 3. Propagate down to compute depth
SELECT tree_id, tree_parent_id, fn, dur, depth
FROM tree_to_table!(
  tree_propagate_down(
    tree_merge_siblings(
      tree_filter(
        tree_from_table!((SELECT * FROM calls), (fn, dur, ones)),
        tree_where(tree_constraint('dur', '>=', 10))
      ),
      tree_group('fn'),
      tree_agg('dur', 'SUM'),
      tree_agg('ones', 'SUM')
    ),
    'ones', 'SUM', 'depth'
  ),
  (fn, dur, depth)
)
ORDER BY tree_id;
```

Step by step:

1. **Filter** removes node 5 (alloc, dur=5). 7 nodes remain.
2. **Merge siblings** merges the two children of "parse" and "emit" that don't
   share names, so no merging happens here (alloc and lex are different; alloc
   and write are different). The tree structure is unchanged.
3. **Propagate down** computes depth by summing 1s from root.

| tree_id | tree_parent_id | fn    | dur | depth |
|---------|----------------|-------|-----|-------|
| 0       | NULL           | main  | 100 | 1     |
| 1       | 0              | parse | 60  | 2     |
| 2       | 1              | alloc | 20  | 3     |
| 3       | 1              | lex   | 30  | 3     |
| 4       | 0              | emit  | 35  | 2     |
| 5       | 4              | alloc | 15  | 3     |
| 6       | 4              | write | 15  | 3     |

## Design

Start with a problem, explain how we can solve it with trees. Best to explain to
peopel not used to it.

Overarching principles: organized in multiple layers:

- Top layer is SQL functions for working with trees - inspired heavily by
  SQLite's JSON functions. Of course will have functions to convert between
  trees and tables.
- These stdlib functions are really intrinsic C++ functions which create some
  "lazy" operations on top of trees.
- Translated to trace processor's custom bytecode and run on our interpreter
  - interpreter operates at the scale of full tables/graphs _not_ on the scale
    of individual rows (generally speaking) so is very efficient
- Lowest layer is very low level ops which do small, highly performant operation
  on the tree.

The building blocks for SQL:

- Nodes: a set of arbitrary properties (columns) which have an id (representing
  unique identifier) and parent_id (representing parent in tree).
- Node selection operator: responsible for deciding which nodes are going to be
  processed by higher level oeprations
- Node grouping operator: Responsible for partitioning nodes of a tree into
  "groups" which can then be handled by higher level operations.
- Implicitly "NULL" acts as the hidden root to make it a valid tree (even though
  it's really more accurately a forest if you're pedantic).

Converting between tables and trees is the foundation. `tree_from_table` takes
a table with `id`, `parent_id` and any additional columns, aggregates the rows
and produces an opaque tree object. `tree_to_table` converts back, but with a
crucial transformation: the original (potentially sparse) `id` and `parent_id`
values are normalized to dense row indices stored in `_tree_id` (always 0, 1,
2, ..., n-1) and `_tree_parent_id` (the row index of the parent, NULL for
roots). The original columns are preserved alongside these tree columns. This
normalization enables O(1) parent lookups via direct array indexing - the key
to making tree operations fast.

The SQL operations:

- Propogate down -> take some propreties of a noderun a function on parent
  value, child value (for simplicitly we will just use aggregate functions).
  e.g. computing depth is a sum operation over a column full of 1s
- Propogate up -> same but up tree - also needs a decomposable function to
  "reduce" multiple values coming from children into a single value
- Filter node -> given a node selection, removes the nodes from the graph and
  reparents any children which survive to a surviving parent (or they become
  roots if they don't)
- Invert tree -> given a node grouping, make the leaves become the new roots and
  then merge recursively by the node grouping
- Merge siblings (no ordering) -> given a grouping operator merge all sibling
  nodes together. Does not care about order (i.e. merging is global per parent
  node)
- Merge siblings (with ordering) -> given a grouping operator merge all sibling
  nodes which are part of same group but only _in sequence_. Every time the
  value changes, merging no longer happens
- Merge into parent -> given a grouping operator, if a node is in same group as
  parent, merge into it's parent. Requires giving aggregators for remaining
  columns as well.

These need to be translated to a bunch of very composable bytecodes making use
of all the existing ones we already have to sort/filter etc data.

### Bytecode layer

**Data structures:**
- `ChildToParent`: parent index per node + original_rows mapping back to table.
  This is the in-memory representation of `_tree_id` (implicit, just row index)
  and `_tree_parent_id` columns produced by `tree_to_table`.
- `ParentToChild`: CSR (offsets + children) + roots list for BFS

**Design decisions:**
- NULL parent (roots) = UINT32_MAX
- Memory owned by persistent Tree object; bytecodes use Spans
- Trees always compact: no excess rows, dense indices 0..n-1. The id
  normalization done by `tree_from_table` ensures this invariant.
- CSR rebuilt by caller when stale (after structural changes)
- RwHandle = reuse allocation in place

**Bytecodes for filter operation:**
1. `MakeChildToParentTreeStructure` - table (id, parent_id columns) → ChildToParent
2. `MakeParentToChildTreeStructure` - ChildToParent → ParentToChild (CSR with roots)
3. `IndexSpanToBitvector` - Span<uint32_t> → BitVector (general utility)
4. `FilterTree` - BFS from roots using CSR, compute surviving ancestors, reparent,
   compact ChildToParent

**Filter flow:**
```
Table(id, parent_id)
  → MakeChildToParentTreeStructure → ChildToParent
  → MakeParentToChildTreeStructure → ParentToChild (CSR)

(existing filter bytecodes → Span<uint32_t> of nodes to keep)
  → IndexSpanToBitvector → BitVector

(CSR + BitVector + ChildToParent)
  → FilterTree → updated ChildToParent (compacted, reparented)
```

**Bytecode for propagate down:**
- `PropagateDown<T, AggOp>` - BFS from roots, applies aggregate operation
  - T = storage type (Uint32, Int32, Int64, Double, String)
  - AggOp = Sum, Min, Max, First, Last
  - Input: CSR (for traversal), update_register (pre-initialized with values)
  - For each node: `update[child] = f(update[parent], update[child])`

**Propagate down flow (e.g., compute depth):**
```
(Initialize output column: roots=0, others=1)
  → PropagateDown<Uint32, SumOp>(CSR, output)
  → output now contains depths
```

## Alternatives considered

- Custom syntax -> maybe we will go here some day but needs a SQL parser which
  is rock solid first.

## Open questions

N/A
