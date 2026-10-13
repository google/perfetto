# Graph Operators (`docs/spec.md` §7)

The `perfetto` DuckDB extension implements the PerfettoSQL Next graph operators
(`docs/spec.md` §7) with both **PerfettoSQL Next `|>` pipeline syntax**
(`src/pipe.cc`) and **`pf_graph_*` DuckDB table macros** (`sql/graphs.sql`)
backed by native C++ kernels (`src/graphs.cc`):

| Spec §7 | DuckDB macro | Output columns |
|---|---|---|
| `GRAPH DFS TREE NODES n EDGES e FROM r` | `pf_graph_dfs_tree(nodes, edges, roots, per := [...], full_row := true)` | `nodes.*`, `parent_id BIGINT` (`NULL` at roots) |
| `GRAPH BFS TREE NODES n EDGES e FROM r` | `pf_graph_bfs_tree(nodes, edges, roots, per := [...], full_row := true)` | `nodes.*`, `parent_id BIGINT` (`NULL` at roots) |
| `GRAPH DOMINATOR TREE NODES n EDGES e FROM r` | `pf_graph_dominator_tree(nodes, edges, roots, per := [...], full_row := true)` | `nodes.*`, `parent_id BIGINT`, `depth BIGINT` |

## Loading

```python
import duckdb
con = duckdb.connect(config={'allow_unsigned_extensions': 'true'})
con.execute("LOAD './perfetto.duckdb_extension'")
```

```sql
-- PerfettoSQL Next |> pipeline syntax (in Bigtrace DuckDB SQL mode or FROM pf_pipe('...')):
GRAPH DOMINATOR TREE
  NODES heap_objects
  EDGES heap_refs
  FROM gc_roots
  PER (_trace_uuid, upid, graph_sample_ts)
|> TREE ACCUMULATE UP SUM(self_size) AS retained_bytes, COUNT(*) AS dominated_objs
   PER (_trace_uuid, upid, graph_sample_ts)
|> AGGREGATE SUM(retained_bytes) AS class_retained, COUNT(*) AS instances
   GROUP BY _trace_uuid, class_name
|> ORDER BY class_retained DESC
```

Every relation argument (`nodes`, `edges`, `roots`) in `pf_graph_*` macros is
passed as a string: either a table/view name (`'heap_graph_object'`) or a
parenthesized SQL query (`'(SELECT _trace_uuid, id, self_size FROM
heap_graph_object)'`). In `|>` pipeline syntax, `NODES`, `EDGES`, and `FROM`
accept table/view names or parenthesized subqueries/pipelines `(FROM ... |> ...)`.

---

## Input Schema

| Relation | Required columns | Extra columns |
|---|---|---|
| `nodes` | `id BIGINT`, all `per` columns | Any payload columns (`self_size`, `type_id`, `root_type`, ...); carried onto the output when `full_row := true` |
| `edges` | `src BIGINT`, `dst BIGINT` (or `source_node_id`, `dest_node_id`), all `per` columns | Ignored |
| `roots` | `id BIGINT` (or `root_node_id`), all `per` columns | Ignored |

`per` is a `VARCHAR[]` list of partition columns led by `_trace_uuid` (for
example, `per := ['_trace_uuid', 'upid', 'graph_sample_ts']`). Each distinct
partition is solved as an independent graph in parallel across DuckDB threads.

When `full_row := false`, the macro skips joining `nodes` back and returns only
the `per` columns, `id`, `parent_id` (plus `depth` for `pf_graph_dominator_tree`).

---

## Semantics & Tie-Breaking (Exact Parity with Trace Processor)

All three operators reduce a directed graph `(nodes, edges)` from `roots` to a
tree `(id, parent_id)` containing **only the nodes reachable from `roots`** (with
`id` present in `nodes`). Every tree operator (`pf_tree_accumulate_up`,
`pf_tree_flatten_recursion`, `pf_tree_key_siblings`, `pf_tree_merge_siblings`,
...) composes directly on the result.

1. **Deduplication & dangling references**:
   - Duplicate node `id`s, duplicate edges `(src, dst)`, and duplicate roots are
     deduplicated per partition.
   - Edges whose `src` or `dst` is `NULL` or absent from `nodes`, and roots
     whose `id` is `NULL` or absent from `nodes`, are ignored.
2. **`pf_graph_dfs_tree` (`GRAPH DFS TREE`)**:
   - Seeds are processed in ascending `id` order.
   - Outgoing neighbors of each node are visited in ascending `dst` `id` order
     (standard recursive DFS preorder: the smallest unvisited child is entered
     first, and its entire subtree is completed before the next sibling).
   - A seed already reached from an earlier seed is not re-rooted; otherwise its
     `parent_id` is `NULL`.
3. **`pf_graph_bfs_tree` (`GRAPH BFS TREE`)**:
   - All distinct valid seeds are enqueued at distance 0 in ascending `id`
     order with `parent_id = NULL`.
   - FIFO queue discipline; outgoing neighbors are scanned in ascending `dst`
     `id` order, so among shortest paths from the seed set, ties are broken by
     earliest-discovered parent (and by smaller seed `id` at distance 0).
4. **`pf_graph_dominator_tree` (`GRAPH DOMINATOR TREE`)**:
   - Lengauer–Tarjan immediate dominators over a virtual super-root connected to
     every valid seed in `roots`.
   - The virtual super-root is stripped from the output: any node whose
     immediate dominator is the super-root has `parent_id = NULL` and
     `depth = 0` (children of depth-0 nodes have `depth = 1`, etc.).
   - Notice that a seed reachable from another seed is dominated by the
     super-root (`parent_id = NULL`, `depth = 0`), whereas a non-seed node
     reachable via multiple seeds without a single dominating seed also has
     `parent_id = NULL` and `depth = 0`.

---

## End-to-End Examples: Java Heap Dump Analysis

Perfetto's `heap_graph_object` and `heap_graph_reference` tables can hold
multiple heap dumps per trace, keyed by `(_trace_uuid, upid, graph_sample_ts)`.

### 1. Retained size per object and top classes by retained bytes (`pf_graph_dominator_tree` + `pf_tree_accumulate_up`)

```sql
-- Nodes: heap objects keyed by (_trace_uuid, upid, graph_sample_ts)
CREATE OR REPLACE VIEW heap_nodes AS
SELECT
  _trace_uuid,
  upid,
  graph_sample_ts,
  id,
  type_id,
  self_size
FROM heap_graph_object
WHERE reachable != 0;

-- Edges: object-to-object references (owner_id -> owned_id) within each dump
CREATE OR REPLACE VIEW heap_edges AS
SELECT
  o._trace_uuid,
  o.upid,
  o.graph_sample_ts,
  r.owner_id AS src,
  r.owned_id AS dst
FROM heap_graph_reference AS r
JOIN heap_graph_object AS o
  ON r._trace_uuid = o._trace_uuid
 AND r.owner_id = o.id
WHERE r.owned_id IS NOT NULL;

-- Roots: GC roots (root_type IS NOT NULL)
CREATE OR REPLACE VIEW heap_roots AS
SELECT
  _trace_uuid,
  upid,
  graph_sample_ts,
  id
FROM heap_graph_object
WHERE root_type IS NOT NULL;

-- 1a. Dominator tree across all traces and heap dumps in parallel:
CREATE OR REPLACE TABLE dom_tree AS
SELECT * FROM pf_graph_dominator_tree(
  'heap_nodes',
  'heap_edges',
  'heap_roots',
  per := ['_trace_uuid', 'upid', 'graph_sample_ts']
);

-- 1b. Per-object retained size (subtree sum of self_size in the dominator tree):
CREATE OR REPLACE TABLE object_retained AS
SELECT
  d._trace_uuid,
  d.upid,
  d.graph_sample_ts,
  d.id,
  d.parent_id,
  d.depth,
  d.type_id,
  d.self_size,
  r.total AS retained_size
FROM dom_tree AS d
JOIN pf_tree_accumulate_up(
  'dom_tree',
  'self_size',
  per := ['_trace_uuid', 'upid', 'graph_sample_ts']
) AS r
  USING (_trace_uuid, upid, graph_sample_ts, id);

-- 1c. Top classes by root-level retained bytes (summing over dominator roots of each class,
-- or after collapsing same-class dominator chains with pf_tree_merge_into_parent):
WITH class_dom AS (
  SELECT
    d._trace_uuid,
    d.upid,
    d.graph_sample_ts,
    d.id,
    d.parent_id,
    COALESCE(c.deobfuscated_name, c.name) AS class_name,
    d.self_size
  FROM dom_tree AS d
  JOIN heap_graph_class AS c
    ON d._trace_uuid = c._trace_uuid
   AND d.type_id = c.id
),
collapsed_class_dom AS (
  SELECT * FROM pf_tree_merge_into_parent(
    'class_dom',
    ['class_name'],
    ['SUM(self_size) AS self_size'],
    indirect := true,
    per := ['_trace_uuid', 'upid', 'graph_sample_ts']
  )
),
class_retained AS (
  SELECT
    c._trace_uuid,
    c.upid,
    c.graph_sample_ts,
    c.id,
    c.class_name,
    c.self_size,
    a.total AS retained_size
  FROM collapsed_class_dom AS c
  JOIN pf_tree_accumulate_up(
    'collapsed_class_dom',
    'self_size',
    per := ['_trace_uuid', 'upid', 'graph_sample_ts']
  ) AS a
    USING (_trace_uuid, upid, graph_sample_ts, id)
)
SELECT
  _trace_uuid,
  upid,
  graph_sample_ts,
  class_name,
  COUNT(*) AS instance_clusters,
  SUM(self_size) AS shallow_bytes,
  SUM(retained_size) AS retained_bytes
FROM class_retained
GROUP BY _trace_uuid, upid, graph_sample_ts, class_name
ORDER BY retained_bytes DESC;
```

### 2. Shortest retention paths (`pf_graph_bfs_tree` + `pf_tree_flatten_recursion` + `pf_tree_key_siblings`)

Build a shortest-hop GC-root retention tree with `pf_graph_bfs_tree`, unroll
same-class cycles (`LinkedList$Node -> LinkedList$Node`) with
`pf_tree_flatten_recursion`, and group identical class paths with
`pf_tree_key_siblings`:

```sql
WITH bfs_objects AS (
  SELECT
    b._trace_uuid,
    b.upid,
    b.graph_sample_ts,
    b.id,
    b.parent_id,
    COALESCE(c.deobfuscated_name, c.name) AS class_name,
    b.self_size
  FROM pf_graph_bfs_tree(
    'heap_nodes',
    'heap_edges',
    'heap_roots',
    per := ['_trace_uuid', 'upid', 'graph_sample_ts']
  ) AS b
  JOIN heap_graph_class AS c
    ON b._trace_uuid = c._trace_uuid
   AND b.type_id = c.id
),
unrolled AS (
  SELECT * FROM pf_tree_flatten_recursion(
    'bfs_objects',
    ['class_name'],
    per := ['_trace_uuid', 'upid', 'graph_sample_ts']
  )
),
keyed AS (
  SELECT * FROM pf_tree_key_siblings(
    'unrolled',
    ['class_name'],
    key_col := 'path_id',
    parent_key_col := 'parent_path_id',
    per := ['_trace_uuid', 'upid', 'graph_sample_ts']
  )
)
SELECT
  _trace_uuid,
  upid,
  graph_sample_ts,
  path_id,
  parent_path_id,
  class_name,
  COUNT(*) AS object_count,
  SUM(self_size) AS shallow_bytes
FROM keyed
GROUP BY _trace_uuid, upid, graph_sample_ts, path_id, parent_path_id, class_name
ORDER BY shallow_bytes DESC;
```

---

## Architecture & Algorithm (`src/graphs.cc`)

Each operator expands to a single native DuckDB aggregate function
`pf_graph_tree_agg(kind, role, a, b)` grouped by the `per` columns (`GROUP BY
_trace_uuid, upid, graph_sample_ts`).

1. **Streamed tagged union**:
   `nodes` (`role = 0`), `edges` (`role = 1`), and `roots` (`role = 2`) are
   streamed into `pf_graph_tree_agg` via `UNION ALL`. DuckDB's parallel radix
   hash-aggregate partitions the stream by `per` and calls `GraphFinalize` in
   parallel across partitions.
2. **Adaptive ID mapping (`IdMap`)**:
   Distinct node `id`s are mapped to dense indices `0 .. N-1` in ascending `id`
   order (`index(u) < index(v) ⇔ id(u) < id(v)`). When `max(id) - min(id) <=
   4 * N + 1024`, a direct flat array (`O(1)` lookup, zero hashing) is used;
   when `id`s are contiguous `lo .. lo + N - 1` (standard TP `heap_graph_object`
   IDs `0 .. N-1`), the lookup table is elided completely (`index = id - lo`).
   Sparse `id`s fall back to a power-of-two open-addressed hash table.
3. **Two-pass CSR construction**:
   Outgoing edges (and incoming edges for `DOMINATOR`) are packed into a
   compact Compressed Sparse Row (`out_off`, `out_adj`) buffer. Adjacency lists
   are sorted lazily on first visit of each reachable node.
4. **Lengauer–Tarjan Dominators (`O(E α(E, V))`)**:
   - Iterative DFS from the virtual super-root `0` assigns discovery times
     `dfn[1..T]` (`T <= N + 1`).
   - Reverse-preorder pass computes semidominators (`sdom`) and immediate
     dominators (`idom`) using path-compressing `Eval`/`Link` (`anc`, `best`)
     and per-semidominator intrusive buckets (`bhead`, `bnext`) — zero heap
     allocations inside the loop.
   - Forward pass resolves `idom[w]` and computes `depth[w]` (`depth = 0` for
     children of the super-root).

---

## Verification

```bash
python3 tests/test_graphs.py
```

- **Unit & property tests**: empty relations, single node, missing/dangling
  endpoints, duplicate nodes/edges/roots, self-loops, cycles, diamond and
  classic Lengauer–Tarjan flowgraphs, disconnected components, forest of roots,
  tie-breaking order, negative and sparse 64-bit IDs, payload preservation, and
  multi-partition (`per`) isolation.
- **Randomized differential fuzzing**: 300 random directed graphs across sizes,
  densities, and seed counts checked for exact equality against independent
  Python reference implementations of DFS, BFS, and bitset-intersection
  dominators.
- **Trace Processor oracle**: exact row-level equality of `(id, parent_id)` (and
  `depth` for dominator) against Trace Processor's `GRAPH DFS TREE`,
  `GRAPH BFS TREE`, and `GRAPH DOMINATOR TREE` on real Android Java heap dumps.
