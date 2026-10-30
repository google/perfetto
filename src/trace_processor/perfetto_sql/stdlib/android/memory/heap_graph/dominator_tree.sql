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
-- The heap graph dominator tree is calculated by GRAPH DOMINATOR TREE.
-- Each reachable object is a node in the dominator tree, their immediate
-- dominator is their parent node in the tree, and their dominated set is all
-- their descendants in the tree. All size information come from the
-- heap_graph_object prelude table.
CREATE PERFETTO TABLE heap_graph_dominator_tree(
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
)
AS
GRAPH DOMINATOR TREE
  NODES (SELECT id, self_size, native_size FROM heap_graph_object)
  EDGES (
    -- Edges out of reachable objects, minus the referent of weak, phantom and
    -- finalizer references.
    SELECT ref.owner_id AS src, ref.owned_id AS dst
    FROM heap_graph_object AS o
    CROSS JOIN heap_graph_reference AS ref USING (reference_set_id)
    WHERE
      o.reachable
      AND o.reference_set_id IS NOT NULL
      AND ref.owned_id IS NOT NULL
      AND NOT (o.type_id IN _ref_type_ids
      AND ref.field_name = 'java.lang.ref.Reference.referent')
  )
  FROM (SELECT id FROM heap_graph_object WHERE root_type IS NOT NULL)
|> TREE ACCUMULATE UP
  COUNT(*) AS dominated_obj_count,
  SUM(self_size) AS dominated_size_bytes,
  SUM(native_size) AS dominated_native_size_bytes
|> TREE ACCUMULATE DOWN COUNT(*) AS depth
|> SELECT
  id,
  parent_id AS idom_id,
  dominated_obj_count,
  dominated_size_bytes,
  dominated_native_size_bytes,
  depth
|> ORDER BY id;
