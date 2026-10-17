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

-- The dominator class tree (see dominator_class_tree.sql) with the field
-- holding every object as a frame between its immediate dominator and its
-- class. Totals match _heap_graph_dominator_class_tree.
--
-- Most objects are referenced directly by their immediate dominator, and the
-- frame is that field (e.g. '.mPackages'). The others are retained by their
-- dominator through several paths (a diamond): no field of the dominator holds
-- them, so the frame instead names the field which holds them on the shortest
-- path, e.g. '[via HashMap$Node.value]'.

INCLUDE PERFETTO MODULE android.memory.heap_graph.field_tree;

INCLUDE PERFETTO MODULE android.memory.heap_graph.field_tree_helpers;

INCLUDE PERFETTO MODULE android.memory.heap_graph.helpers;

INCLUDE PERFETTO MODULE android.memory.heap_graph.raw_dominator_tree;

CREATE PERFETTO TABLE _heap_graph_dominator_field_tree_edges AS
SELECT
  t.id,
  t.idom_id AS parent_id,
  p.type_id IN _heap_graph_array_class AS parent_is_array
FROM _raw_heap_graph_dominator_tree AS t
JOIN heap_graph_object AS p ON t.idom_id = p.id;

CREATE PERFETTO INDEX _heap_graph_dominator_field_tree_edges_idx ON _heap_graph_dominator_field_tree_edges(
  parent_id,
  id
);

-- Dominator edges which are references. Absent edges cross a diamond.
CREATE PERFETTO TABLE _heap_graph_dominator_field_tree_edge_refs AS
FROM (
  SELECT *
  FROM _heap_graph_tree_edge_ref_candidates!(
    _heap_graph_dominator_field_tree_edges
  )
)
|> AGGREGATE MIN(ref_id) AS ref_id GROUP BY id;

CREATE PERFETTO INDEX _heap_graph_dominator_field_tree_edge_refs_idx ON _heap_graph_dominator_field_tree_edge_refs(
  id
);

CREATE PERFETTO TABLE _heap_graph_dominator_field_tree_nodes AS
WITH
  labelled AS (
    SELECT
      t.id,
      t.idom_id AS parent_id,
      o.upid,
      o.graph_sample_ts,
      o.type_id,
      k.name_hash,
      o.root_type,
      o.heap_type,
      o.self_size,
      o.native_size,
      d.id IS NOT NULL AS is_direct,
      iif(d.id IS NOT NULL, d.ref_id, s.ref_id) AS ref_id
    FROM _raw_heap_graph_dominator_tree AS t
    JOIN heap_graph_object AS o USING (id)
    JOIN _heap_graph_class_name_hash AS k ON o.type_id = k.id
    LEFT JOIN _heap_graph_dominator_field_tree_edge_refs AS d USING (id)
    LEFT JOIN _heap_graph_field_tree_edge_refs AS s USING (id)
  )
SELECT
  l.id,
  l.parent_id,
  l.upid,
  l.graph_sample_ts,
  l.type_id,
  l.name_hash,
  l.root_type,
  l.heap_type,
  l.self_size,
  l.native_size,
  CASE
    WHEN l.parent_id IS NULL THEN NULL
    WHEN l.is_direct THEN iif(r.id IS NULL, NULL, 0)
    WHEN r.id IS NULL THEN 2
    ELSE 1
  END AS edge_kind,
  iif(
    l.parent_id IS NULL,
    NULL,
    coalesce(r.deobfuscated_field_name, r.field_name)
  ) AS field_name,
  iif(l.parent_id IS NULL, NULL, r.field_type_name) AS field_type_name
FROM labelled AS l
LEFT JOIN heap_graph_reference AS r ON l.ref_id = r.id;

CREATE PERFETTO INDEX _heap_graph_dominator_field_tree_nodes_idx ON _heap_graph_dominator_field_tree_nodes(
  id
);

CREATE PERFETTO TABLE _heap_graph_dominator_field_tree_runs AS
SELECT *
FROM _heap_graph_field_tree_runs!(_heap_graph_dominator_field_tree_nodes);

CREATE PERFETTO INDEX _heap_graph_dominator_field_tree_runs_idx ON _heap_graph_dominator_field_tree_runs(
  id
);

CREATE PERFETTO TABLE _heap_graph_dominator_field_tree_path_hashes AS
SELECT *
FROM _heap_graph_field_path_hash!(
  _heap_graph_dominator_field_tree_nodes,
  _heap_graph_dominator_field_tree_runs
);

CREATE PERFETTO INDEX _heap_graph_dominator_field_tree_path_hashes_idx ON _heap_graph_dominator_field_tree_path_hashes(
  id
);

-- Objects and the path hashes of the dominator field tree frames they belong
-- to.
CREATE PERFETTO TABLE _heap_graph_dominator_field_path_hashes AS
SELECT *
FROM _heap_graph_field_path_hash_objects!(
  _heap_graph_dominator_field_tree_path_hashes
)
ORDER BY
  path_hash;

CREATE PERFETTO TABLE _heap_graph_dominator_field_path_hashes_aggregated AS
SELECT *
FROM _heap_graph_field_path_hash_aggregate!(
  _heap_graph_dominator_field_tree_nodes,
  _heap_graph_dominator_field_tree_path_hashes
);

CREATE PERFETTO INDEX _heap_graph_dominator_field_path_hashes_aggregated_idx ON _heap_graph_dominator_field_path_hashes_aggregated(
  path_hash
);

-- The dominator field tree: one row per frame, with the same columns as
-- _heap_graph_field_tree.
CREATE PERFETTO TABLE _heap_graph_dominator_field_tree AS
SELECT *
FROM _heap_graph_field_path_hashes_to_tree!(
  _heap_graph_dominator_field_path_hashes_aggregated
);
