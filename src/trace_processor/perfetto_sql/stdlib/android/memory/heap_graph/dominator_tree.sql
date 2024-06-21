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

INCLUDE PERFETTO MODULE graphs.dominator_tree;

-- Excluding following types from the graph as they share objects' ownership
-- with their real (more interesting) owners and will mask their idom to be the
-- "super root".
CREATE PERFETTO TABLE _ref_type_ids AS
SELECT id AS type_id FROM heap_graph_class
WHERE kind IN (
  'KIND_FINALIZER_REFERENCE',
  'KIND_PHANTOM_REFERENCE',
  'KIND_SOFT_REFERENCE',
  'KIND_WEAK_REFERENCE');

CREATE PERFETTO TABLE _excluded_refs AS
SELECT ref.id
  FROM _ref_type_ids
  JOIN heap_graph_object robj USING (type_id)
  JOIN heap_graph_reference ref USING (reference_set_id)
WHERE ref.field_name = 'java.lang.ref.Reference.referent'
ORDER BY ref.id;

-- The assigned id of the "super root".
-- Since a Java heap graph is a "forest" structure, we need to add a imaginary
-- "super root" node which connects all the roots of the forest into a single
-- connected component, so that the dominator tree algorithm can be performed.
CREATE PERFETTO FUNCTION _heap_graph_super_root_fn()
-- The assigned id of the "super root".
RETURNS INT AS
SELECT id + 1
FROM heap_graph_object
ORDER BY id DESC
LIMIT 1;

CREATE PERFETTO VIEW _dominator_compatible_heap_graph AS
SELECT
  ref.owner_id AS source_node_id,
  ref.owned_id AS dest_node_id
FROM heap_graph_reference ref
JOIN heap_graph_object source_node ON ref.owner_id = source_node.id
WHERE source_node.reachable
  AND ref.id NOT IN _excluded_refs
  AND ref.owned_id IS NOT NULL
UNION ALL
SELECT
  (SELECT _heap_graph_super_root_fn()) as source_node_id,
  id AS dest_node_id
FROM heap_graph_object
WHERE root_type IS NOT NULL;

CREATE PERFETTO TABLE _idom_ordered_heap_graph_dominator_tree AS
SELECT node_id AS id, dominator_node_id as idom_id
FROM graph_dominator_tree!(
  _dominator_compatible_heap_graph,
  (SELECT _heap_graph_super_root_fn())
)
-- Excluding the imaginary root.
WHERE dominator_node_id IS NOT NULL
-- Ordering by idom_id so queries below are faster when joining on idom_id.
-- TODO(lalitm): support create index for Perfetto tables.
ORDER BY idom_id;

CREATE PERFETTO TABLE _heap_graph_dominator_tree AS
SELECT
  id,
  IIF(
    idom_id = _heap_graph_super_root_fn(),
    NULL,
    idom_id
  ) AS idom_id
FROM _idom_ordered_heap_graph_dominator_tree
ORDER BY id;

CREATE PERFETTO TABLE _heap_graph_dominator_tree_depth AS
WITH RECURSIVE _tree_visitor(id, depth) AS (
  -- Let the super root have depth 0.
  SELECT id, 1 AS depth
  FROM _idom_ordered_heap_graph_dominator_tree
  WHERE idom_id = _heap_graph_super_root_fn()
  UNION ALL
  SELECT child.id, parent.depth + 1
  FROM _idom_ordered_heap_graph_dominator_tree child
  JOIN _tree_visitor parent ON child.idom_id = parent.id
)
SELECT * FROM _tree_visitor
ORDER BY id;

-- A performance note: we need 3 memoize functions because EXPERIMENTAL_MEMOIZE
-- limits the function to return only 1 int.
-- This means the exact same "memoized dfs pass" on the tree is done 3 times, so
-- it takes 3x the time taken by only doing 1 pass. Doing only 1 pass would be
-- possible if EXPERIMENTAL_MEMOIZE could return more than 1 int.

CREATE PERFETTO FUNCTION _subtree_obj_count(id INT)
RETURNS INT AS
SELECT 1 + IFNULL((
  SELECT
    SUM(_subtree_obj_count(child.id))
  FROM _idom_ordered_heap_graph_dominator_tree child
  WHERE child.idom_id = $id
), 0);
SELECT EXPERIMENTAL_MEMOIZE('_subtree_obj_count');

CREATE PERFETTO FUNCTION _subtree_size_bytes(id INT)
RETURNS INT AS
SELECT (
  SELECT self_size
  FROM heap_graph_object
  WHERE heap_graph_object.id = $id
) +
IFNULL((
  SELECT
    SUM(_subtree_size_bytes(child.id))
  FROM _idom_ordered_heap_graph_dominator_tree child
  WHERE child.idom_id = $id
), 0);
SELECT EXPERIMENTAL_MEMOIZE('_subtree_size_bytes');

CREATE PERFETTO FUNCTION _subtree_native_size_bytes(id INT)
RETURNS INT AS
SELECT (
  SELECT native_size
  FROM heap_graph_object
  WHERE heap_graph_object.id = $id
) +
IFNULL((
  SELECT
    SUM(_subtree_native_size_bytes(child.id))
  FROM _idom_ordered_heap_graph_dominator_tree child
  WHERE child.idom_id = $id
), 0);
SELECT EXPERIMENTAL_MEMOIZE('_subtree_native_size_bytes');

-- All reachable heap graph objects, their immediate dominators and summary of
-- their dominated sets.
-- The heap graph dominator tree is calculated by stdlib graphs.dominator_tree.
-- Each reachable object is a node in the dominator tree, their immediate
-- dominator is their parent node in the tree, and their dominated set is all
-- their descendants in the tree. All size information come from the
-- heap_graph_object prelude table.
CREATE PERFETTO TABLE heap_graph_dominator_tree(
  -- Heap graph object id.
  id INT,
  -- Immediate dominator object id of the object. If the immediate dominator
  -- is the "super-root" (i.e. the object is a root or is dominated by multiple
  -- roots) then `idom_id` will be NULL.
  idom_id INT,
  -- Count of all objects dominated by this object, self inclusive.
  dominated_obj_count INT,
  -- Total self_size of all objects dominated by this object, self inclusive.
  dominated_size_bytes INT,
  -- Total native_size of all objects dominated by this object, self inclusive.
  dominated_native_size_bytes INT,
  -- Depth of the object in the dominator tree. Depth of root objects are 1.
  depth INT
) AS
SELECT
  t.id,
  t.idom_id,
  _subtree_obj_count(t.id) AS dominated_obj_count,
  _subtree_size_bytes(t.id) AS dominated_size_bytes,
  _subtree_native_size_bytes(t.id) AS dominated_native_size_bytes,
  d.depth
FROM _heap_graph_dominator_tree t
JOIN _heap_graph_dominator_tree_depth d USING(id)
ORDER BY id;
