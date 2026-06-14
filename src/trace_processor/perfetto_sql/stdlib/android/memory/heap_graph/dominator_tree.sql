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

-- All reachable heap graph objects, their immediate dominators and summary of
-- their dominated sets.
-- The heap graph dominator tree is calculated by stdlib graphs.dominator_tree.
-- Each reachable object is a node in the dominator tree, their immediate
-- dominator is their parent node in the tree, and their dominated set is all
-- their descendants in the tree. All size information come from the
-- heap_graph_object prelude table.
CREATE PERFETTO PIPELINE heap_graph_dominator_tree(
  -- Heap graph object id.
  id LONG,
  -- Immediate dominator object id of the object. If the immediate dominator
  -- is the "super-root" (i.e. the object is a root or is dominated by multiple
  -- roots) then `idom_id` will be NULL.
  idom_id LONG,
  -- Count of all objects dominated by this object, self inclusive.
  dominated_obj_count LONG,
  -- Total self_size of all objects dominated by this object, self inclusive.
  dominated_size_bytes LONG,
  -- Total native_size of all objects dominated by this object, self inclusive.
  dominated_native_size_bytes LONG,
  -- Depth of the object in the dominator tree. Depth of root objects are 1.
  depth LONG
) MATERIALIZED AS
-- Node payload: every reachable heap object, carrying its sizes.
SUBPIPELINE nodes AS (
  FROM heap_graph_object
  |> WHERE reachable
  |> SELECT id, self_size, native_size
)
-- Edges: ownership references between reachable objects, minus excluded refs.
SUBPIPELINE edges AS (
  FROM heap_graph_reference AS ref
  |> JOIN heap_graph_object AS source_node ON ref.owner_id = source_node.id
  |> WHERE source_node.reachable
       AND ref.id NOT IN _excluded_refs
       AND ref.owned_id IS NOT NULL
  |> SELECT ref.owner_id AS source_node_id, ref.owned_id AS dest_node_id
)
-- Roots: the GC roots of the forest. The dominator operator inserts a virtual
-- super-root over these and strips it from the result, so each object dominated
-- by the super-root has a NULL idom_id.
SUBPIPELINE roots AS (
  FROM heap_graph_object
  |> WHERE root_type IS NOT NULL
  |> SELECT id
)
GRAPH DOMINATOR TREE NODES nodes EDGES edges FROM roots
-- The immediate dominator of a node is its parent in the dominator tree; NULL
-- for objects dominated by the (stripped) super-root.
|> RENAME parent_id AS idom_id
-- Dominated-set summaries: subtree sums (self inclusive) of count and sizes.
|> TREE ACCUMULATE UP
   COUNT(*) AS dominated_obj_count,
   SUM(self_size) AS dominated_size_bytes,
   SUM(native_size) AS dominated_native_size_bytes
-- Depth: length of the root-path. Root objects have depth 1.
|> TREE ACCUMULATE DOWN COUNT(*) AS depth
|> SELECT
     id,
     idom_id,
     dominated_obj_count,
     dominated_size_bytes,
     dominated_native_size_bytes,
     depth
|> ORDER BY id;
