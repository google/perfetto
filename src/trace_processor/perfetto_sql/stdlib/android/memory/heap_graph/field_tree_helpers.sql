--
-- Copyright 2026 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed ON an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- Building blocks for the "field trees": class trees (see class_tree.sql) in
-- which the edge from a parent object to a child object is labelled with the
-- field of the parent which holds the child. A field tree interleaves class and
-- field frames (Class -> .field -> Class -> ...), so a flamegraph of it shows
-- *how* memory is retained, not just which classes retain it.
--
-- A field tree is built from a table of "labelled nodes": one row per object
-- with its tree parent and the edge to it, with the columns
--   id, parent_id, upid, graph_sample_ts, type_id, name_hash, root_type,
--   heap_type, self_size, native_size,
--   edge_kind: how the parent holds the object (see below),
--   field_name: the declaring class qualified name of the field,
--   field_type_name: the declared type of the field.
--
-- `edge_kind` is NULL when there is no field frame (roots and array elements)
-- and otherwise one of:
--   0: the parent references the object through `field_name`;
--   1: the parent does not reference the object (a dominator across a
--      diamond) and `field_name` is the field which does on the shortest path;
--   2: as 1, but no field references the object on the shortest path.

INCLUDE PERFETTO MODULE graphs.scan;

-- The text after the last '.' of `s` (all of `s` if it has no '.').
-- `rtrim` with every non-'.' character of `s` strips back to the last '.'.
CREATE PERFETTO MACRO _heap_graph_last_segment(s Expr)
RETURNS Expr
AS substr($s, length(rtrim($s, replace($s, '.', ''))) + 1);

-- The text before the last '.' of `s` ('' if it has no '.').
CREATE PERFETTO MACRO _heap_graph_before_last_segment(s Expr)
RETURNS Expr
AS rtrim(rtrim($s, replace($s, '.', '')), '.');

-- The frame name of an edge: '.field' when the parent holds the object in
-- that field, '[via Owner.field]' when another object does.
CREATE PERFETTO MACRO _heap_graph_edge_label(edge_kind Expr, field_name Expr)
RETURNS Expr
AS CASE $edge_kind
  WHEN 0 THEN '.' || _heap_graph_last_segment!($field_name)
  WHEN 1 THEN '[via ' || _heap_graph_last_segment!(
      _heap_graph_before_last_segment!($field_name)
    ) || '.' || _heap_graph_last_segment!($field_name) || ']'
  ELSE '[indirect]'
END;

-- Classes whose instances are arrays. Their outgoing references are elements,
-- not fields: proto heap dumps name them '' and hprof ones '[0]', '[1]', ...
CREATE PERFETTO TABLE _heap_graph_array_class AS
SELECT id
FROM heap_graph_class
WHERE
  coalesce(deobfuscated_name, name, '') GLOB '*[]'
ORDER BY
  id;

-- Given an indexed table of tree edges `edges` (id, parent_id,
-- parent_is_array), returns a row for every reference backing an edge, with
-- the id of the reference if it names a field of the parent (`ref_id`, NULL
-- for array elements). Edges which are not references (e.g. dominator edges
-- across a diamond) have no rows. Aggregate with MIN(ref_id) per id.
--
-- The references are scanned once and probed into `edges`: probing the other
-- way is quadratic for large arrays, which share one reference set.
CREATE PERFETTO MACRO _heap_graph_tree_edge_ref_candidates(
  edges TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SELECT
    e.id,
    iif(r.field_name != '' AND NOT e.parent_is_array, r.id, NULL) AS ref_id
  FROM heap_graph_reference AS r
  CROSS JOIN $edges AS e
    ON e.parent_id = r.owner_id AND e.id = r.owned_id
);

-- Given labelled nodes `nodes`, returns for every node which continues a run
-- of nodes with the same class and edge as their parent the parent of the
-- first node of the run (`run_parent_id`). E.g. every element of a linked list
-- after the first maps to the parent of the first one. Re-parenting a run that
-- way turns it into siblings, so it collapses into a single `.next -> Node`
-- level instead of a frame per element. Runs never start at a root.
CREATE PERFETTO MACRO _heap_graph_field_tree_runs(nodes TableOrSubquery)
RETURNS TableOrSubquery
AS (
  WITH
    run_edges AS (
      SELECT
        c.parent_id AS source_node_id,
        c.id AS dest_node_id,
        p.parent_id AS source_parent_id
      FROM $nodes AS c
      JOIN $nodes AS p
        ON c.parent_id = p.id
      WHERE
        c.name_hash = p.name_hash
        AND c.edge_kind IS p.edge_kind
        AND c.field_name IS p.field_name
        AND p.parent_id IS NOT NULL
    )
  SELECT id, run_parent_id
  FROM _graph_scan!(
    run_edges,
    (
      SELECT DISTINCT
        source_node_id AS id,
        source_parent_id AS run_parent_id
      FROM run_edges
      WHERE
        source_node_id NOT IN (SELECT dest_node_id FROM run_edges)
    ),
    (run_parent_id),
    (SELECT id, run_parent_id FROM $table)
  )
);

-- Computes for every labelled node of `nodes`, after re-parenting the runs in
-- `runs` (see _heap_graph_field_tree_runs), the hash of its path from the root
-- (`path_hash`) alternating class and field frames, the hash of the field
-- frame above it (`field_path_hash`, 0 without one) and the hash of its parent
-- (`parent_path_hash`, 0 for roots). Root hashes match the ones of
-- _heap_graph_type_path_hash.
CREATE PERFETTO MACRO _heap_graph_field_path_hash(
  nodes TableOrSubquery,
  runs TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SELECT id, path_hash, field_path_hash, parent_path_hash
  FROM _graph_scan!(
    (
      SELECT
        coalesce(r.run_parent_id, n.parent_id) AS source_node_id,
        n.id AS dest_node_id
      FROM $nodes AS n
      LEFT JOIN $runs AS r
        USING (id)
      WHERE
        n.parent_id IS NOT NULL
    ),
    (
      SELECT
        id,
        HASH(
          upid,
          graph_sample_ts,
          name_hash,
          ifnull(root_type, ''),
          ifnull(heap_type, '')
        ) AS path_hash,
        0 AS field_path_hash,
        0 AS parent_path_hash
      FROM $nodes
      WHERE
        parent_id IS NULL
    ),
    (path_hash, field_path_hash, parent_path_hash),
    -- The step query is stringified: string literals must use double quotes.
    (
      SELECT
        t.id,
        HASH(
          iif(
            n.edge_kind IS NULL,
            t.path_hash,
            HASH(t.path_hash, n.edge_kind, ifnull(n.field_name, ""))
          ),
          n.name_hash,
          ifnull(n.heap_type, "")
        ) AS path_hash,
        iif(
          n.edge_kind IS NULL,
          0,
          HASH(t.path_hash, n.edge_kind, ifnull(n.field_name, ""))
        ) AS field_path_hash,
        t.path_hash AS parent_path_hash
      FROM $table AS t
      JOIN $nodes AS n
        USING (id)
    )
  )
);

-- Maps every object to the path hashes of the frames it belongs to: its class
-- frame and, if it has one, the field frame above it. Used to list the objects
-- behind a frame.
CREATE PERFETTO MACRO _heap_graph_field_path_hash_objects(
  path_hashes TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SELECT id, path_hash
  FROM $path_hashes
  UNION ALL
  SELECT id, field_path_hash AS path_hash
  FROM $path_hashes
  WHERE
    field_path_hash != 0
);

-- Aggregates labelled nodes `nodes` with path hashes `path_hashes` into one
-- row per frame: field frames (no size of their own), class frames and, under
-- class frames with native memory, a '[native]' frame.
CREATE PERFETTO MACRO _heap_graph_field_path_hash_aggregate(
  nodes TableOrSubquery,
  path_hashes TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  WITH
    hashed AS (
      SELECT
        n.graph_sample_ts,
        n.upid,
        n.type_id,
        n.root_type,
        n.heap_type,
        n.self_size,
        n.native_size,
        n.edge_kind,
        n.field_name,
        n.field_type_name,
        h.path_hash,
        h.field_path_hash,
        h.parent_path_hash
      FROM $nodes AS n
      JOIN $path_hashes AS h
        USING (id)
    ),
    classes AS (
      SELECT
        graph_sample_ts,
        upid,
        path_hash,
        iif(
          field_path_hash != 0,
          field_path_hash,
          nullif(parent_path_hash, 0)
        ) AS parent_path_hash,
        type_id,
        root_type,
        heap_type,
        count() AS self_count,
        sum(iif(self_size != -1, self_size, 0)) AS self_size,
        sum(native_size > 0) AS self_native_count,
        sum(native_size) AS self_native_size
      FROM hashed
      GROUP BY
        path_hash
    ),
    named_classes AS (
      SELECT
        x.*,
        coalesce(c.deobfuscated_name, c.name, '') AS name
      FROM classes AS x
      JOIN heap_graph_class AS c
        ON x.type_id = c.id
    ),
    fields AS (
      SELECT
        graph_sample_ts,
        upid,
        field_path_hash AS path_hash,
        parent_path_hash,
        edge_kind,
        field_name,
        field_type_name
      FROM hashed
      WHERE
        field_path_hash != 0
      GROUP BY
        field_path_hash
    )
  SELECT
    graph_sample_ts,
    upid,
    path_hash,
    parent_path_hash,
    _heap_graph_edge_label!(edge_kind, field_name) AS name,
    'field' AS node_kind,
    NULL AS root_type,
    NULL AS heap_type,
    field_name,
    field_type_name,
    0 AS self_count,
    0 AS self_size
  FROM fields
  UNION ALL
  SELECT
    graph_sample_ts,
    upid,
    path_hash,
    parent_path_hash,
    name,
    'class' AS node_kind,
    root_type,
    heap_type,
    NULL AS field_name,
    NULL AS field_type_name,
    self_count,
    self_size
  FROM named_classes
  UNION ALL
  SELECT
    graph_sample_ts,
    upid,
    HASH(path_hash, 'native', '') AS path_hash,
    path_hash AS parent_path_hash,
    '[native] ' || name AS name,
    'native' AS node_kind,
    root_type,
    'HEAP_TYPE_NATIVE' AS heap_type,
    NULL AS field_name,
    NULL AS field_type_name,
    self_native_count AS self_count,
    self_native_size AS self_size
  FROM named_classes
  WHERE
    self_native_size > 0
  ORDER BY
    path_hash
);

-- Converts the per-frame aggregate `tab` (see
-- _heap_graph_field_path_hash_aggregate) into a tree with ids. `tab` must be a
-- Perfetto table indexed on path_hash.
CREATE PERFETTO MACRO _heap_graph_field_path_hashes_to_tree(tab TableOrSubquery)
RETURNS TableOrSubquery
AS (
  SELECT
    graph_sample_ts,
    upid,
    _auto_id AS id,
    (
      SELECT p._auto_id
      FROM $tab AS p
      WHERE
        c.parent_path_hash = p.path_hash
    ) AS parent_id,
    name,
    node_kind,
    root_type,
    heap_type,
    field_name,
    field_type_name,
    self_count,
    self_size,
    path_hash AS path_hash_stable
  FROM $tab AS c
  ORDER BY
    id
);
