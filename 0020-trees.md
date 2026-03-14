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
