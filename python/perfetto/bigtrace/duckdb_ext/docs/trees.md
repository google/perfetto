# Tree Operators

The `perfetto` DuckDB extension implements 12 tree
operators with both **`|>` pipe syntax** (`src/pipe.cc`) and
**`pf_tree_*` DuckDB table macros** (`sql/trees.sql`) backed by C++ aggregate
kernels (`src/trees.cc`).

## Loading

```python
import duckdb
con = duckdb.connect(config={'allow_unsigned_extensions': 'true'})
con.execute("LOAD './perfetto.duckdb_extension'")  # built by ../build.sh
```

```sql
-- |> pipe syntax (in Bigtrace DuckDB SQL mode or FROM pf_pipe('...')):
FROM slice AS s
|> WHERE dur >= 0
|> TREE ACCUMULATE UP SUM(dur) AS subtree_dur, COUNT(*) AS subtree_nodes PER (_trace_uuid)
|> TREE KEEP IF DESCENDANT OF (
     FROM slice |> WHERE name = 'Choreographer#doFrame'
   ) PER (_trace_uuid)
|> SELECT _trace_uuid, id, parent_id, name, dur, subtree_dur, subtree_nodes

-- Equivalent single-trace query (just drop PER (_trace_uuid)):
FROM slice AS s
|> WHERE dur >= 0
|> TREE ACCUMULATE UP SUM(dur) AS subtree_dur, COUNT(*) AS subtree_nodes
|> TREE KEEP IF DESCENDANT OF (
     FROM slice |> WHERE name = 'Choreographer#doFrame'
   )
|> SELECT id, parent_id, name, dur, subtree_dur, subtree_nodes
```

## Input contract

- Relations are passed as a table/view name (`'slice'`) or a parenthesized
  query string (`'(SELECT id, parent_id, dur FROM slice)'`).
- Every tree relation has `id BIGINT NOT NULL` and `parent_id BIGINT` (`NULL` =
  root).
- `per := ['_trace_uuid', ...]` partitions the input into independent forests
  (one per trace, solved in parallel across DuckDB threads).
- Errors match Trace Processor:
  - `NULL` in `id` → `"a row has no id"`
  - duplicate `id` within a partition → `"more than one row has the same node"`
  - `parent_id` not present as an `id` in the same partition (on operators that
    require the complete tree: `accumulate`, `key_siblings`, `merge_siblings`,
    `merge_into_parent`, `invert`, `expand`) → `"a row names a parent which is
    not itself a row in the input"`
  - cycle in `parent_id` → `"a node is its own parent"` or `"the rows are not a
    tree, because N of them are in a cycle"`
- Aggregate specifications are SQL strings `'AGG(expr) AS name'` using the
  vocabulary: `COUNT(*)`, `COUNT(x)`, `SUM(x)`, `MIN(x)`, `MAX(x)`, `AVG(x)`,
  `ARG_MAX(val, ord)`, `ARG_MIN(val, ord)`, `GROUP_CONCAT(x)` (and `FIRST(x)`,
  `LAST(x)` on `pf_tree_accumulate(..., 'down', ...)`).

## Operator Summary

| Pipe stage | DuckDB macro |
|---|---|
| `TREE KEEP IF (ANCESTOR\|DESCENDANT) OF rel [OVER o]` | `pf_tree_keep_if(tbl, 'ancestor'\|'descendant', marker, per := [...], over := NULL, full_row := true)` |
| `TREE DROP IF (ANCESTOR\|DESCENDANT) OF rel [OVER o]` | `pf_tree_drop_if(tbl, 'ancestor'\|'descendant', marker, per := [...], over := NULL, full_row := true)` |
| `TREE CONTRACT AT rel [OVER o] AGGREGATE ...` | `pf_tree_contract(tbl, marker, aggs := [...], per := [...], over := NULL)` |
| `TREE ABSORB AT rel [OVER o] AGGREGATE ...` | `pf_tree_absorb(tbl, marker, aggs := [...], per := [...], over := NULL)` |
| `TREE PRUNE AT rel [OVER o]` | `pf_tree_prune(tbl, marker, per := [...], over := NULL, full_row := true)` |
| `TREE REPARENT TO NEAREST ANCESTOR IN rel [OVER o]` | `pf_tree_reparent(tbl, marker, per := [...], over := NULL)` |
| `TREE FLATTEN RECURSION BY cols [OVER o]` | `pf_tree_flatten_recursion(tbl, by_cols, per := [...], over := NULL)` |
| `TREE KEY SIBLINGS BY cols AS (k, pk)` | `pf_tree_key_siblings(tbl, by_cols, key_col := 'key', parent_key_col := 'parent_key', per := [...])` |
| `TREE MERGE SIBLINGS BY cols [ORDERED] AGGREGATE ...` | `pf_tree_merge_siblings(tbl, by_cols, aggs := [...], ordered := false, per := [...])` |
| `TREE MERGE INTO PARENT BY cols [INDIRECT] AGGREGATE ...` | `pf_tree_merge_into_parent(tbl, by_cols, aggs := [...], indirect := false, per := [...])` |
| `TREE INVERT` | `pf_tree_invert(tbl, weight_col := 'self_weight', per := [...])` |
| `TREE ACCUMULATE (UP\|DOWN) agg AS name, ...` | `pf_tree_accumulate(tbl, 'up'\|'down', aggs, per := [...], full_row := true)` |
| Fast single-column `SUM`: `(per..., id, total)` | `pf_tree_accumulate_up(tbl, col, per := [...])`, `pf_tree_accumulate_down(tbl, col, per := [...])` |
| `TREE EXPAND (UP\|DOWN) rel BY cols [ORDERED BY o] [CHARGE c TO LEAF\|ROOT]` | `pf_tree_expand(tbl, 'up'\|'down', rel, by_cols, ordered_by := [...], charge_cols := [...], charge_to := NULL, per := [...])` |

---

## Accumulation (`pf_tree_accumulate_up`, `pf_tree_accumulate_down`, `pf_tree_accumulate`)

### Inclusive subtree duration and exclusive self-time per slice

In Perfetto's `slice` table, `dur` is each slice's wall duration (`ts` to `ts +
dur`), which already includes any child slices on the same track. You can also
accumulate a per-node weight bottom-up across subtrees with
`pf_tree_accumulate_up`, or compute exclusive self-time (`dur` minus the sum of
direct children's `dur`, or `2 * s.dur - a.total` on a non-overlapping slice
tree where `a.total` is the subtree sum of `dur`):

```sql
-- Fast lean subtree sum: returns (_trace_uuid, id, total)
SELECT
  _trace_uuid,
  id,
  total AS subtree_sum_dur
FROM pf_tree_accumulate_up('slice', 'dur', per := ['_trace_uuid']);

-- Root-to-node path sum: returns (_trace_uuid, id, total)
SELECT
  _trace_uuid,
  id,
  total AS path_sum_dur
FROM pf_tree_accumulate_down('slice', 'dur', per := ['_trace_uuid']);

-- Exclusive self-time per slice by subtracting direct children's duration:
WITH child_dur AS (
  SELECT
    _trace_uuid,
    parent_id AS id,
    SUM(dur) AS children_dur
  FROM slice
  WHERE parent_id IS NOT NULL
  GROUP BY _trace_uuid, parent_id
),
slice_self AS (
  SELECT
    s._trace_uuid,
    s.id,
    s.parent_id,
    s.name,
    s.dur,
    s.dur - COALESCE(c.children_dur, 0) AS self_dur
  FROM slice AS s
  LEFT JOIN child_dur AS c
    USING (_trace_uuid, id)
)
-- Verify that accumulating self_dur UP recovers each slice's inclusive dur:
SELECT
  s._trace_uuid,
  s.id,
  s.name,
  s.dur AS inclusive_dur,
  s.self_dur AS exclusive_self_dur,
  a.total AS accumulated_self_dur
FROM slice_self AS s
JOIN pf_tree_accumulate_up('slice_self', 'self_dur', per := ['_trace_uuid']) AS a
  USING (_trace_uuid, id);
```

### Multi-aggregate `pf_tree_accumulate` (`UP` and `DOWN`)

`pf_tree_accumulate` supports multiple aggregates in one pass (`SUM`, `COUNT`,
`MIN`, `MAX`, `AVG`, `ARG_MAX`, `ARG_MIN`, `GROUP_CONCAT`, plus `FIRST` and
`LAST` when `direction = 'down'`). Pass `full_row := false` when only `per`,
`id`, and the aggregate columns are needed (only available when all aggregates
are `SUM`).

```sql
-- Subtree (UP) aggregation with multiple aggregates:
SELECT
  _trace_uuid,
  id,
  name,
  subtree_dur,
  subtree_count,
  max_child_dur,
  slowest_descendant
FROM pf_tree_accumulate(
  'slice',
  'up',
  [
    'SUM(dur) AS subtree_dur',
    'COUNT(*) AS subtree_count',
    'MAX(dur) AS max_child_dur',
    'ARG_MAX(name, dur) AS slowest_descendant'
  ],
  per := ['_trace_uuid']
);

-- Root-to-node path (DOWN) aggregation: breadcrumb path, root name, depth
SELECT
  _trace_uuid,
  id,
  name,
  root_slice_name,
  nearest_tag,
  call_path,
  path_nodes
FROM pf_tree_accumulate(
  'slice',
  'down',
  [
    'FIRST(name) AS root_slice_name',
    'LAST(category) AS nearest_tag',
    'GROUP_CONCAT(name) AS call_path',
    'COUNT(*) AS path_nodes'
  ],
  per := ['_trace_uuid']
);
```

---

## Filtering (`pf_tree_keep_if`, `pf_tree_drop_if`, `pf_tree_prune`)

`pf_tree_keep_if` and `pf_tree_drop_if` filter nodes by structural relationship
to a `marker` set of `(per..., id)` rows:
- `'descendant'`: strict descendants of any marker row (and the marker rows
  themselves in `keep_if`? Note: in the pipe syntax, `DESCENDANT OF` and
  `ANCESTOR OF` are **strict**—use `pf_tree_prune` to drop markers and their
  descendants together, or include markers via `over`).
- `'ancestor'`: strict ancestors of any marker row.
- `over`: optional complete tree relation when filtering an already-subsetted
  working set.

```sql
-- Keep strict descendants under 'Choreographer#doFrame' slices:
SELECT
  _trace_uuid,
  id,
  parent_id,
  name,
  dur
FROM pf_tree_keep_if(
  'slice',
  'descendant',
  '(SELECT _trace_uuid, id FROM slice WHERE name = ''Choreographer#doFrame'')',
  per := ['_trace_uuid']
);

-- Keep strict ancestors leading up to 'binder transaction' slices:
SELECT
  _trace_uuid,
  id,
  parent_id,
  name,
  dur
FROM pf_tree_keep_if(
  'slice',
  'ancestor',
  '(SELECT _trace_uuid, id FROM slice WHERE name = ''binder transaction'')',
  per := ['_trace_uuid']
);

-- Drop strict descendants of 'LockContention' slices:
SELECT * FROM pf_tree_drop_if(
  'slice',
  'descendant',
  '(SELECT _trace_uuid, id FROM slice WHERE name = ''LockContention'')',
  per := ['_trace_uuid']
);

-- Prune 'DebugLog' slices and their entire subtrees (surviving children reparent):
SELECT * FROM pf_tree_prune(
  'slice',
  '(SELECT _trace_uuid, id FROM slice WHERE name = ''DebugLog'')',
  per := ['_trace_uuid']
);
```

---

## Reshaping & Reparenting (`pf_tree_contract`, `pf_tree_absorb`, `pf_tree_reparent`, `pf_tree_flatten_recursion`)

### `pf_tree_contract` and `pf_tree_absorb`

- `pf_tree_contract(tbl, marker, aggs, per := [...])`: removes each non-root
  marker node, reparents its children to its nearest surviving ancestor, and
  folds its measures (`aggs`) into that ancestor.
- `pf_tree_absorb(tbl, marker, aggs, per := [...])`: keeps each marker node,
  drops all of its descendants, and folds their measures (`aggs`) into the
  marker node.

```sql
-- Contract internal helper frames, charging their self_dur to their parent:
SELECT * FROM pf_tree_contract(
  'callstack_slices',
  '(SELECT _trace_uuid, id FROM callstack_slices WHERE name LIKE ''%__internal%'')',
  ['SUM(self_dur) AS self_dur'],
  per := ['_trace_uuid']
);

-- Absorb subtrees under 'jni_call' so 'jni_call' becomes a leaf with total subtree self_dur:
SELECT * FROM pf_tree_absorb(
  'callstack_slices',
  '(SELECT _trace_uuid, id FROM callstack_slices WHERE name = ''jni_call'')',
  ['SUM(self_dur) AS self_dur'],
  per := ['_trace_uuid']
);
```

### `pf_tree_reparent` and `pf_tree_flatten_recursion`

- `pf_tree_reparent(tbl, marker, per := [...])`: row-preserving; sets each
  node's `parent_id` to its nearest strict ancestor in `marker` (`NULL` if
  none).
- `pf_tree_flatten_recursion(tbl, by_cols, per := [...])`: row-preserving; any
  node whose `by_cols` match its parent's becomes its parent's sibling
  (repeatedly up the chain, so `A -> A -> A` becomes three siblings under `A`'s
  parent).

```sql
-- Reparent every slice to its nearest top-level UI/binder milestone ancestor:
SELECT * FROM pf_tree_reparent(
  'slice',
  '(SELECT _trace_uuid, id FROM slice WHERE category IN (''ui'', ''binder''))',
  per := ['_trace_uuid']
);

-- Unroll direct self-recursion by slice name before building a flamegraph:
SELECT * FROM pf_tree_flatten_recursion(
  'slice',
  ['name'],
  per := ['_trace_uuid']
);
```

---

## Callstacks & Flamegraphs (`pf_tree_key_siblings`, `pf_tree_merge_siblings`, `pf_tree_merge_into_parent`, `pf_tree_invert`, `pf_tree_expand`)

### `pf_tree_key_siblings` and `pf_tree_merge_siblings`

Both operators key every node by its root-to-node path of `by_cols`:
- `pf_tree_key_siblings`: row-preserving; appends dense `key` and `parent_key`
  (`NULL` for roots) columns so nodes sharing the same root-to-node `by_cols`
  path receive the same `key`.
- `pf_tree_merge_siblings`: collapses nodes sharing the same root-to-node
  `by_cols` path into a single flamegraph node, aggregating measures via `aggs`
  (or only consecutive runs when `ordered := true`).

```sql
-- Tag every slice with its callstack path key (node_id, parent_node_id):
SELECT
  _trace_uuid,
  id,
  parent_id,
  name,
  node_id,
  parent_node_id
FROM pf_tree_key_siblings(
  'slice',
  ['name'],
  key_col := 'node_id',
  parent_key_col := 'parent_node_id',
  per := ['_trace_uuid']
);

-- Build a per-trace flamegraph by merging siblings with the same root->name path:
WITH merged AS (
  SELECT * FROM pf_tree_merge_siblings(
    'callstack_slices',
    ['name'],
    ['SUM(self_dur) AS self_dur', 'COUNT(*) AS call_count'],
    per := ['_trace_uuid']
  )
)
SELECT * FROM pf_tree_accumulate(
  'merged',
  'up',
  ['SUM(self_dur) AS cumulative_dur'],
  per := ['_trace_uuid']
);
```

### `pf_tree_merge_into_parent`

Collapses recursive frames into their matching ancestor (`indirect := false`
collapses direct `A -> A` child into parent; `indirect := true` also folds
mutual recursion `A -> B -> A` and intermediate nodes into the topmost `A`):

```sql
SELECT * FROM pf_tree_merge_into_parent(
  'callstack_slices',
  ['name'],
  ['SUM(self_dur) AS self_dur'],
  indirect := true,
  per := ['_trace_uuid']
);
```

### `pf_tree_invert`

Flips leaves and roots to produce a bottom-up (heavy-leaf-first) call-tree /
flamegraph, propagating `weight_col` along reversed paths:

```sql
SELECT
  _trace_uuid,
  id,
  parent_id,
  name,
  self_dur
FROM pf_tree_invert(
  'callstack_slices',
  weight_col := 'self_dur',
  per := ['_trace_uuid']
);
```

### `pf_tree_expand`

Inserts a keyed sub-structure `rel` (e.g., inline stack frames for each
`callsite_id`) below (`'down'`) or above (`'up'`) each matched node, optionally
migrating `charge_cols` to the far end (`'leaf'` for `'down'`, `'root'` for
`'up'`):

```sql
SELECT * FROM pf_tree_expand(
  'callsites',
  'down',
  'inline_frames',
  by_cols := ['callsite_id'],
  ordered_by := ['inline_depth'],
  charge_cols := ['self_dur'],
  charge_to := 'leaf',
  per := ['_trace_uuid']
);
```
