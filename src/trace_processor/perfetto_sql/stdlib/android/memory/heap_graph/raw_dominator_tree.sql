--
-- Copyright 2024 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

INCLUDE PERFETTO MODULE android.memory.heap_graph.excluded_refs;

CREATE PERFETTO PIPELINE _raw_heap_graph_dominator_tree MATERIALIZED AS
SUBPIPELINE nodes AS (
  FROM heap_graph_object
  |> SELECT id
)
SUBPIPELINE edges AS (
  FROM heap_graph_reference AS ref
  |> JOIN heap_graph_object AS source_node ON ref.owner_id = source_node.id
  |> WHERE
       source_node.reachable
       AND ref.id NOT IN _excluded_refs
       AND ref.owned_id IS NOT NULL
  |> SELECT ref.owner_id AS source_node_id, ref.owned_id AS dest_node_id
)
-- A Java heap graph is a "forest" structure: the multi-root FROM provides the GC
-- roots, and GRAPH DOMINATOR TREE inserts (and strips) a virtual super-root over
-- them so no id is user-synthesized.
SUBPIPELINE roots AS (
  FROM heap_graph_object
  |> WHERE root_type IS NOT NULL
  |> SELECT id
)
GRAPH DOMINATOR TREE NODES nodes EDGES edges FROM roots
|> SELECT id, parent_id AS idom_id
|> WHERE idom_id IS NOT NULL
|> ORDER BY id;

CREATE PERFETTO INDEX _raw_heap_graph_dominator_tree_idom_id_idx ON _raw_heap_graph_dominator_tree(
  idom_id
);
