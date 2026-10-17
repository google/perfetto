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

-- The shortest path class tree (see class_tree.sql) with the field holding
-- every object as a frame between its parent and its class:
-- SystemServer -> .mPackageManagerService -> PackageManagerService -> ...
-- Totals match _heap_graph_class_tree.

INCLUDE PERFETTO MODULE android.memory.heap_graph.class_tree;

INCLUDE PERFETTO MODULE android.memory.heap_graph.field_tree_helpers;

INCLUDE PERFETTO MODULE android.memory.heap_graph.helpers;

CREATE PERFETTO TABLE _heap_graph_field_tree_edges AS
SELECT
  t.id,
  t.parent_id,
  p.type_id IN _heap_graph_array_class AS parent_is_array
FROM _heap_graph_object_min_depth_tree AS t
JOIN heap_graph_object AS p ON t.parent_id = p.id;

CREATE PERFETTO INDEX _heap_graph_field_tree_edges_idx ON _heap_graph_field_tree_edges(
  parent_id,
  id
);

-- Every shortest path tree edge is a reference, so every object below a root
-- gets the field of its parent which holds it (none for array elements).
CREATE PERFETTO TABLE _heap_graph_field_tree_edge_refs AS
FROM (
  SELECT *
  FROM _heap_graph_tree_edge_ref_candidates!(_heap_graph_field_tree_edges)
)
|> AGGREGATE MIN(ref_id) AS ref_id GROUP BY id;

CREATE PERFETTO INDEX _heap_graph_field_tree_edge_refs_idx ON _heap_graph_field_tree_edge_refs(
  id
);

CREATE PERFETTO TABLE _heap_graph_field_tree_nodes AS
SELECT
  t.id,
  t.parent_id,
  o.upid,
  o.graph_sample_ts,
  o.type_id,
  k.name_hash,
  o.root_type,
  o.heap_type,
  o.self_size,
  o.native_size,
  iif(r.id IS NULL, NULL, 0) AS edge_kind,
  coalesce(r.deobfuscated_field_name, r.field_name) AS field_name,
  r.field_type_name
FROM _heap_graph_object_min_depth_tree AS t
JOIN heap_graph_object AS o USING (id)
JOIN _heap_graph_class_name_hash AS k ON o.type_id = k.id
LEFT JOIN _heap_graph_field_tree_edge_refs AS e USING (id)
LEFT JOIN heap_graph_reference AS r ON e.ref_id = r.id;

CREATE PERFETTO INDEX _heap_graph_field_tree_nodes_idx ON _heap_graph_field_tree_nodes(
  id
);

CREATE PERFETTO TABLE _heap_graph_field_tree_runs AS
SELECT * FROM _heap_graph_field_tree_runs!(_heap_graph_field_tree_nodes);

CREATE PERFETTO INDEX _heap_graph_field_tree_runs_idx ON _heap_graph_field_tree_runs(
  id
);

CREATE PERFETTO TABLE _heap_graph_field_tree_path_hashes AS
SELECT *
FROM _heap_graph_field_path_hash!(
  _heap_graph_field_tree_nodes,
  _heap_graph_field_tree_runs
);

CREATE PERFETTO INDEX _heap_graph_field_tree_path_hashes_idx ON _heap_graph_field_tree_path_hashes(
  id
);

-- Objects and the path hashes of the field tree frames they belong to.
CREATE PERFETTO TABLE _heap_graph_field_path_hashes AS
SELECT *
FROM _heap_graph_field_path_hash_objects!(_heap_graph_field_tree_path_hashes)
ORDER BY
  path_hash;

CREATE PERFETTO TABLE _heap_graph_field_path_hashes_aggregated AS
SELECT *
FROM _heap_graph_field_path_hash_aggregate!(
  _heap_graph_field_tree_nodes,
  _heap_graph_field_tree_path_hashes
);

CREATE PERFETTO INDEX _heap_graph_field_path_hashes_aggregated_idx ON _heap_graph_field_path_hashes_aggregated(
  path_hash
);

-- The field tree: one row per frame. `node_kind` is 'class', 'field' or
-- 'native'. Field frames are named after the field (e.g. '.mPackages'), carry
-- the declaring class qualified `field_name` and `field_type_name`, and have no
-- size of their own.
CREATE PERFETTO TABLE _heap_graph_field_tree AS
SELECT *
FROM _heap_graph_field_path_hashes_to_tree!(
  _heap_graph_field_path_hashes_aggregated
);
