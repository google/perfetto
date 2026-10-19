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

-- Java collections in heap graphs and the fields holding them.
--
-- Collections are objects whose class implements java.util.Collection or
-- java.util.Map (from its Class object's ifTable) or has a recipe in
-- android.memory.heap_graph.collection_recipes: the recipe of the nearest
-- class in the superclass chain locates the elements. Element counts are the
-- non-null elements referenced (entries for maps). Reference arrays not owned
-- by a collection are collections too (layout 'array').
--
-- Ownership (internal objects, retained sizes, wrappers) follows the dominator
-- tree, except that a GC root with a single referrer is owned by it: ART
-- reports most zygote and boot image objects as roots.

INCLUDE PERFETTO MODULE android.memory.heap_graph.class_relationship;

INCLUDE PERFETTO MODULE android.memory.heap_graph.class_tree;

INCLUDE PERFETTO MODULE android.memory.heap_graph.collection_recipes;

INCLUDE PERFETTO MODULE android.memory.heap_graph.excluded_refs;

INCLUDE PERFETTO MODULE android.memory.heap_graph.object_tree;

INCLUDE PERFETTO MODULE android.memory.heap_graph.raw_dominator_tree;

INCLUDE PERFETTO MODULE graphs.scan;

INCLUDE PERFETTO MODULE graphs.search;

-- One row per field listed in a recipe. `pos` is the position in its list:
-- for maps, element field 0 holds the key and 1 the value.
CREATE PERFETTO TABLE _heap_graph_collection_recipe_field AS
WITH
  -- Recipe field lists have at most 16 fields.
  pos(i) AS (
    SELECT 0
    UNION ALL
    SELECT i + 1 FROM pos WHERE i < 15
  ),
  lists AS (
    SELECT class_name, 'storage' AS role, storage_field AS fields
    FROM _heap_graph_collection_recipe
    UNION ALL
    SELECT class_name, 'link', link_fields FROM _heap_graph_collection_recipe
    UNION ALL
    SELECT class_name, 'element', element_fields
    FROM _heap_graph_collection_recipe
    UNION ALL
    SELECT class_name, 'internal', internal_fields
    FROM _heap_graph_collection_recipe
    UNION ALL
    SELECT class_name, 'sentinel', sentinel_field
    FROM _heap_graph_collection_recipe
  )
SELECT
  class_name AS recipe_class,
  role,
  i AS pos,
  str_split(fields, ',', i) AS field_name
FROM lists
JOIN pos
WHERE
  str_split(fields, ',', i) IS NOT NULL;

-- The recipe of every class: the one of the nearest class in its superclass
-- chain that has one.
CREATE PERFETTO TABLE _heap_graph_collection_class AS
WITH
  recipe_class AS (
    SELECT c.id, r.class_name AS recipe_class
    FROM heap_graph_class AS c
    JOIN _heap_graph_collection_recipe AS r
      ON r.class_name = coalesce(c.deobfuscated_name, c.name)
  ),
  candidate AS (
    SELECT d.id, d.ancestor_class_id
    FROM android_heap_graph_class_find_descendants!((SELECT id FROM recipe_class)) AS d
  ),
  -- The number of recipe classes in the superclass chain of each recipe class,
  -- itself included: of the recipe classes of a class, the nearest has most.
  depth AS (
    SELECT id, count() AS n
    FROM candidate
    WHERE
      id IN (SELECT id FROM recipe_class)
    GROUP BY
      id
  ),
  ranked AS (
    SELECT
      c.id,
      rc.recipe_class,
      row_number() OVER (PARTITION BY c.id ORDER BY d.n DESC) AS rank
    FROM candidate AS c
    JOIN depth AS d
      ON d.id = c.ancestor_class_id
    JOIN recipe_class AS rc
      ON rc.id = c.ancestor_class_id
  )
SELECT id, recipe_class FROM ranked WHERE rank = 1 ORDER BY id;

-- Classes implementing java.util.Collection or java.util.Map, from the
-- interface Class objects in the ifTable of each Class object.
CREATE PERFETTO TABLE _heap_graph_collection_interface_class AS
WITH
  iftable AS (
    SELECT
      substr(
        coalesce(c.deobfuscated_name, c.name),
        17,
        length(coalesce(c.deobfuscated_name, c.name)) - 17
      ) AS class_name,
      a.reference_set_id
    FROM heap_graph_reference AS r
    JOIN heap_graph_object AS o
      ON r.owner_id = o.id
    JOIN heap_graph_class AS c
      ON o.type_id = c.id
    JOIN heap_graph_object AS a
      ON r.owned_id = a.id
    WHERE
      r.field_name = 'java.lang.Class.ifTable'
  ),
  interface AS (
    SELECT
      t.class_name,
      substr(
        coalesce(c.deobfuscated_name, c.name),
        17,
        length(coalesce(c.deobfuscated_name, c.name)) - 17
      ) AS name
    FROM iftable AS t
    JOIN heap_graph_reference AS r
      ON r.reference_set_id = t.reference_set_id
    JOIN heap_graph_object AS o
      ON r.owned_id = o.id
    JOIN heap_graph_class AS c
      ON o.type_id = c.id
    WHERE
      c.name GLOB 'java.lang.Class<java.util.*>'
  )
SELECT
  class_name,
  CASE
    WHEN max(name = 'java.util.Map') THEN 'map'
    WHEN max(name = 'java.util.List') THEN 'list'
    WHEN max(name = 'java.util.Set') THEN 'set'
    WHEN max(name = 'java.util.Queue') THEN 'queue'
    ELSE 'collection'
  END AS kind
FROM interface
GROUP BY
  class_name
HAVING
  max(name IN ('java.util.Collection', 'java.util.Map'));

-- Every reachable collection object, before wrappers and views are folded
-- into the collections they belong to.
CREATE PERFETTO TABLE _heap_graph_collection_object AS
SELECT
  o.id,
  o.upid,
  o.graph_sample_ts,
  coalesce(c.deobfuscated_name, c.name) AS class_name,
  cc.recipe_class,
  ifnull(r.layout, 'unsupported') AS layout,
  coalesce(r.kind, i.kind) AS kind
FROM heap_graph_object AS o
JOIN heap_graph_class AS c
  ON o.type_id = c.id
LEFT JOIN _heap_graph_collection_class AS cc
  ON cc.id = o.type_id
LEFT JOIN _heap_graph_collection_recipe AS r
  ON r.class_name = cc.recipe_class
LEFT JOIN _heap_graph_collection_interface_class AS i
  ON i.class_name = coalesce(c.deobfuscated_name, c.name)
WHERE
  o.reachable
  AND (cc.id IS NOT NULL OR i.class_name IS NOT NULL)
ORDER BY
  o.id;

CREATE PERFETTO INDEX _heap_graph_collection_object_idx ON _heap_graph_collection_object(
  id
);

-- Every non-null reference through a recipe field, from a single pass over
-- the references.
CREATE PERFETTO TABLE _heap_graph_collection_ref AS
SELECT r.owner_id, r.owned_id, f.recipe_class, f.role, f.pos
FROM heap_graph_reference AS r
JOIN _heap_graph_collection_recipe_field AS f
  ON f.field_name = coalesce(r.deobfuscated_field_name, r.field_name)
WHERE
  r.owned_id IS NOT NULL;

CREATE PERFETTO INDEX _heap_graph_collection_ref_owner_idx ON _heap_graph_collection_ref(
  owner_id
);

-- The storage of each collection: an array, a head or root node, or the
-- collection a wrapper delegates to.
CREATE PERFETTO TABLE _heap_graph_collection_storage AS
SELECT
  c.id AS collection_id,
  c.layout,
  r.owned_id AS storage_id,
  s.reference_set_id,
  iif(
    sc.name GLOB '*[]',
    CASE
      WHEN sc.name IN ('long[]', 'double[]') THEN (s.self_size - 16) / 8
      WHEN sc.name IN ('char[]', 'short[]') THEN (s.self_size - 12) / 2
      WHEN sc.name IN ('byte[]', 'boolean[]') THEN s.self_size - 12
      ELSE (s.self_size - 12) / 4
    END,
    NULL
  ) AS length
FROM _heap_graph_collection_object AS c
JOIN _heap_graph_collection_ref AS r
  ON r.owner_id = c.id
  AND r.recipe_class = c.recipe_class
JOIN heap_graph_object AS s
  ON s.id = r.owned_id
JOIN heap_graph_class AS sc
  ON s.type_id = sc.id
WHERE
  r.role = 'storage';

CREATE PERFETTO INDEX _heap_graph_collection_storage_idx ON _heap_graph_collection_storage(
  collection_id
);

-- Reference arrays which are not some collection's storage: reported as
-- collections of their own. Runtime metadata (arrays held by Class and
-- DexCache objects, e.g. Class.ifTable) is left out.
CREATE PERFETTO TABLE _heap_graph_collection_raw_array AS
WITH
  runtime_array AS (
    SELECT DISTINCT owned_id AS id
    FROM heap_graph_reference
    WHERE
      field_name GLOB 'java.lang.Class.*'
      OR field_name GLOB 'java.lang.DexCache.*'
  )
SELECT
  o.id,
  o.upid,
  o.graph_sample_ts,
  coalesce(c.deobfuscated_name, c.name) AS class_name,
  o.reference_set_id,
  (o.self_size - 12) / 4 AS length
FROM heap_graph_object AS o
JOIN heap_graph_class AS c
  ON o.type_id = c.id
WHERE
  o.reachable
  AND c.name GLOB '*[]'
  AND c.name NOT IN (
    'boolean[]',
    'byte[]',
    'char[]',
    'short[]',
    'int[]',
    'long[]',
    'float[]',
    'double[]'
  )
  AND o.id NOT IN (SELECT storage_id FROM _heap_graph_collection_storage)
  AND o.id NOT IN runtime_array
ORDER BY
  o.id;

CREATE PERFETTO INDEX _heap_graph_collection_raw_array_idx ON _heap_graph_collection_raw_array(
  id
);

-- The arrays whose slots are read: storage arrays and raw arrays.
CREATE PERFETTO TABLE _heap_graph_collection_array AS
SELECT collection_id, reference_set_id
FROM _heap_graph_collection_storage
WHERE
  layout IN ('slots', 'slot_pairs', 'nodes')
  AND length IS NOT NULL
UNION ALL
SELECT id, reference_set_id FROM _heap_graph_collection_raw_array;

CREATE PERFETTO INDEX _heap_graph_collection_array_idx ON _heap_graph_collection_array(
  reference_set_id
);

-- The non-null slots of those arrays. Proto heap graphs keep null slots, so a
-- slot's index is its offset in the reference set; hprof ones drop them and
-- name slots '[i]'.
CREATE PERFETTO TABLE _heap_graph_collection_slot AS
SELECT
  a.collection_id,
  iif(
    r.field_name GLOB '[[]*]',
    CAST(substr(r.field_name, 2, length(r.field_name) - 2) AS INT),
    r.id - r.reference_set_id
  ) AS slot,
  r.owned_id
FROM heap_graph_reference AS r
CROSS JOIN _heap_graph_collection_array AS a
  ON r.reference_set_id = a.reference_set_id
WHERE
  r.owned_id IS NOT NULL;

-- The nodes of node collections: the objects reached from the storage (the
-- array's slots, or the head node) through the recipe's link fields.
CREATE PERFETTO TABLE _heap_graph_collection_node AS
WITH
  head AS (
    SELECT DISTINCT s.owned_id AS id, s.collection_id
    FROM _heap_graph_collection_slot AS s
    JOIN _heap_graph_collection_storage AS st USING (collection_id)
    WHERE
      st.layout = 'nodes'
    UNION
    SELECT storage_id, collection_id
    FROM _heap_graph_collection_storage
    WHERE
      layout = 'nodes'
      AND length IS NULL
  ),
  link AS (
    SELECT owner_id AS source_node_id, owned_id AS dest_node_id
    FROM _heap_graph_collection_ref
    WHERE
      role = 'link'
  ),
  -- A DFS forest from the heads: unlike following the links, it terminates on
  -- cycles (e.g. dequeued queue nodes pointing to themselves).
  forest AS (
    SELECT node_id, parent_node_id
    FROM graph_reachable_dfs!(link, (SELECT DISTINCT id AS node_id FROM head))
  )
SELECT DISTINCT id, collection_id
FROM _graph_scan!(
  (
    SELECT parent_node_id AS source_node_id, node_id AS dest_node_id
    FROM forest
    WHERE
    parent_node_id IS NOT NULL
  ),
  head,
  (collection_id),
  (SELECT id, collection_id FROM $table)
);

CREATE PERFETTO INDEX _heap_graph_collection_node_idx ON _heap_graph_collection_node(
  id
);

-- The elements of every collection which has its own storage (not wrappers),
-- with `part` 'key' or 'value' for maps, minus the recipe's sentinels.
CREATE PERFETTO TABLE _heap_graph_collection_own_element AS
WITH
  element AS (
    SELECT
      s.collection_id,
      s.owned_id AS element_id,
      s.slot AS slot_index,
      NULL AS node_id,
      CASE
        WHEN c.layout = 'slot_pairs' THEN iif(s.slot % 2 = 0, 'key', 'value')
        WHEN c.kind = 'map' THEN 'value'
      END AS part
    FROM _heap_graph_collection_slot AS s
    LEFT JOIN _heap_graph_collection_object AS c
      ON c.id = s.collection_id
    -- Slots of node tables hold nodes, not elements.
    WHERE
      ifnull(c.layout, 'array') != 'nodes'
    UNION ALL
    SELECT
      n.collection_id,
      r.owned_id,
      NULL,
      n.id,
      iif(c.kind = 'map', iif(r.pos = 0, 'key', 'value'), NULL)
    FROM _heap_graph_collection_node AS n
    JOIN _heap_graph_collection_object AS c
      ON c.id = n.collection_id
    JOIN _heap_graph_collection_ref AS r
      ON r.owner_id = n.id
      AND r.recipe_class = c.recipe_class
    WHERE
      r.role = 'element'
    UNION ALL
    SELECT
      c.id,
      r.owned_id,
      NULL,
      NULL,
      iif(c.kind = 'map', iif(r.pos = 0, 'key', 'value'), NULL)
    FROM _heap_graph_collection_object AS c
    JOIN _heap_graph_collection_ref AS r
      ON r.owner_id = c.id
      AND r.recipe_class = c.recipe_class
    WHERE
      c.layout = 'fields'
      AND r.role = 'element'
  ),
  sentinel AS (
    SELECT DISTINCT r.recipe_class, r.owned_id AS id
    FROM _heap_graph_collection_ref AS r
    WHERE
      r.role = 'sentinel'
  )
SELECT e.*
FROM element AS e
LEFT JOIN _heap_graph_collection_object AS c
  ON c.id = e.collection_id
WHERE
  NOT EXISTS (
    SELECT 1
    FROM sentinel AS s
    WHERE
      s.recipe_class = c.recipe_class
      AND s.id = e.element_id
  );

CREATE PERFETTO INDEX _heap_graph_collection_own_element_idx ON _heap_graph_collection_own_element(
  collection_id
);

-- Objects which are GC roots themselves (ART reports most zygote and boot
-- image objects as VM internal roots) with their only referrer, if they have
-- one.
CREATE PERFETTO TABLE _heap_graph_collection_sole_referrer AS
SELECT r.owned_id AS id, min(r.owner_id) AS parent_id
FROM heap_graph_reference AS r
CROSS JOIN _raw_heap_graph_dominator_tree AS d
  ON d.id = r.owned_id
JOIN heap_graph_object AS o
  ON o.id = r.owner_id
WHERE
  d.idom_id IS NULL
  AND o.reachable
  AND r.owner_id != r.owned_id
  AND r.id NOT IN _excluded_refs
GROUP BY
  r.owned_id
HAVING
  count(DISTINCT r.owner_id) = 1
ORDER BY
  r.owned_id;

-- The owner of every object: its immediate dominator or, for a GC root, its
-- only referrer unless that makes a cycle (the referrer is owned by it).
CREATE PERFETTO TABLE _heap_graph_collection_parent AS
WITH
  candidate AS (
    SELECT d.id, d.idom_id, ifnull(d.idom_id, s.parent_id) AS parent_id
    FROM _raw_heap_graph_dominator_tree AS d
    LEFT JOIN _heap_graph_collection_sole_referrer AS s USING (id)
  ),
  rooted AS (
    SELECT id
    FROM _graph_scan!(
      (
        SELECT parent_id AS source_node_id, id AS dest_node_id
        FROM candidate
        WHERE
        parent_id IS NOT NULL
      ),
      (SELECT id, id AS root_id FROM candidate WHERE parent_id IS NULL),
      (root_id),
      (SELECT id, root_id FROM $table)
    )
  )
SELECT
  id,
  iif(idom_id IS NULL AND id NOT IN rooted, NULL, parent_id) AS parent_id
FROM candidate
ORDER BY
  id;

CREATE PERFETTO INDEX _heap_graph_collection_parent_idx ON _heap_graph_collection_parent(
  id
);

-- Wrapper chains (HashSet -> HashMap, SynchronizedSet -> HashSet ->
-- HashMap), from every wrapper (`start_id`) to every collection it wraps
-- directly or indirectly. `absorbed` is 1 when each wrapped collection on the
-- way is only reachable through the one wrapping it: such collections fold
-- into the wrapper. A shared collection (e.g. Collections.EMPTY_LIST) is on
-- many chains but folds into none.
CREATE PERFETTO TABLE _heap_graph_collection_delegation AS
WITH
  edge AS (
    SELECT st.collection_id AS source_node_id, st.storage_id AS dest_node_id
    FROM _heap_graph_collection_storage AS st
    JOIN _heap_graph_collection_object AS inner
      ON inner.id = st.storage_id
    WHERE
      st.layout = 'delegate'
      AND st.collection_id != st.storage_id
  ),
  start AS (
    SELECT DISTINCT
      source_node_id AS id,
      source_node_id AS start_id,
      1 AS absorbed,
      source_node_id AS prev_id
    FROM edge
  )
SELECT id, start_id, absorbed
FROM _graph_scan!(
  edge,
  start,
  (start_id, absorbed, prev_id),
  (
    SELECT
    t.id,
    t.start_id,
    t.absorbed AND d.parent_id = t.prev_id AS absorbed,
    t.id AS prev_id
    FROM $table AS t
    JOIN _heap_graph_collection_parent AS d
    USING (id)
  )
);

CREATE PERFETTO INDEX _heap_graph_collection_delegation_idx ON _heap_graph_collection_delegation(
  id
);

-- References through internal fields (cached views and the like).
CREATE PERFETTO TABLE _heap_graph_collection_internal_ref AS
SELECT DISTINCT owner_id, owned_id
FROM _heap_graph_collection_ref
WHERE
  role = 'internal'
  AND owner_id != owned_id;

CREATE PERFETTO INDEX _heap_graph_collection_internal_ref_idx ON _heap_graph_collection_internal_ref(
  owner_id
);

-- The collection reporting each collection object: the outermost wrapper it
-- folds into, or itself. A collection folds into at most one wrapper (the one
-- owning it), so it has at most one outermost wrapper.
CREATE PERFETTO TABLE _heap_graph_collection_row_of AS
WITH
  absorbed AS (
    SELECT id, start_id
    FROM _heap_graph_collection_delegation
    WHERE
      absorbed
      AND id != start_id
  ),
  outermost AS (
    SELECT id, min(start_id) AS start_id
    FROM absorbed
    WHERE
      start_id NOT IN (SELECT id FROM absorbed)
    GROUP BY
      id
  )
SELECT c.id, ifnull(o.start_id, c.id) AS row_id
FROM _heap_graph_collection_object AS c
LEFT JOIN outermost AS o USING (id);

CREATE PERFETTO INDEX _heap_graph_collection_row_of_idx ON _heap_graph_collection_row_of(
  id
);

-- The objects which may be part of a reported collection (`row_id`): the
-- collection objects folding into it, their storage and nodes, and the
-- objects reached from those through internal fields.
CREATE PERFETTO TABLE _heap_graph_collection_candidate AS
WITH
  base AS (
    SELECT id, row_id FROM _heap_graph_collection_row_of
    UNION
    SELECT st.storage_id, r.row_id
    FROM _heap_graph_collection_storage AS st
    JOIN _heap_graph_collection_row_of AS r
      ON r.id = st.collection_id
    WHERE
      st.layout != 'delegate'
    UNION
    SELECT n.id, r.row_id
    FROM _heap_graph_collection_node AS n
    JOIN _heap_graph_collection_row_of AS r
      ON r.id = n.collection_id
  ),
  -- Internal fields are followed two levels deep, which is what the recipes
  -- need (ArrayMap.mCollections -> MapCollections.mKeySet).
  hop1 AS (
    SELECT i.owned_id AS id, b.row_id
    FROM base AS b
    JOIN _heap_graph_collection_internal_ref AS i
      ON i.owner_id = b.id
  )
SELECT id, row_id FROM base
UNION
SELECT id, row_id FROM hop1
UNION
SELECT i.owned_id, h.row_id
FROM hop1 AS h
JOIN _heap_graph_collection_internal_ref AS i
  ON i.owner_id = h.id;

CREATE PERFETTO INDEX _heap_graph_collection_candidate_idx ON _heap_graph_collection_candidate(
  id,
  row_id
);

-- The structure of each reported collection: the candidates it owns (see
-- _heap_graph_collection_parent), reached through other candidates.
CREATE PERFETTO TABLE _heap_graph_collection_structure AS
WITH
  owner_edge AS (
    SELECT DISTINCT d.parent_id AS source_node_id, d.id AS dest_node_id
    FROM _heap_graph_collection_parent AS d
    JOIN _heap_graph_collection_candidate AS c USING (id)
    WHERE
      d.parent_id IS NOT NULL
      AND c.id != c.row_id
  )
SELECT DISTINCT id, row_id AS collection_id
FROM _graph_scan!(
  owner_edge,
  (SELECT DISTINCT row_id AS id, row_id FROM _heap_graph_collection_candidate),
  (row_id),
  (
    SELECT t.id, t.row_id
    FROM $table AS t
    JOIN _heap_graph_collection_candidate AS c
    ON c.id = t.id AND c.row_id = t.row_id
  )
);

CREATE PERFETTO INDEX _heap_graph_collection_structure_idx ON _heap_graph_collection_structure(
  id
);

-- The reported collections: collections which are not part of another one,
-- and raw arrays. `count_id` is the collection whose elements are counted:
-- for wrappers, the end of the chain.
CREATE PERFETTO TABLE _heap_graph_collection_row AS
WITH
  chain_end AS (
    SELECT d.start_id, d.id AS end_id
    FROM _heap_graph_collection_delegation AS d
    JOIN _heap_graph_collection_object AS c USING (id)
    WHERE
      c.layout != 'delegate'
  )
SELECT
  c.id,
  c.upid,
  c.graph_sample_ts,
  c.class_name,
  c.recipe_class,
  c.kind,
  c.layout,
  iif(c.layout = 'delegate', e.end_id, c.id) AS count_id
FROM _heap_graph_collection_object AS c
LEFT JOIN chain_end AS e
  ON e.start_id = c.id
WHERE
  NOT EXISTS (
    SELECT 1
    FROM _heap_graph_collection_structure AS s
    WHERE
      s.id = c.id
      AND s.collection_id != c.id
  )
UNION ALL
SELECT id, upid, graph_sample_ts, class_name, NULL, 'array', 'array', id
FROM _heap_graph_collection_raw_array
WHERE
  id NOT IN (SELECT id FROM _heap_graph_collection_structure);

CREATE PERFETTO INDEX _heap_graph_collection_row_idx ON _heap_graph_collection_row(
  id
);

-- The elements of every reported collection. A set or list wrapping a map
-- (HashSet, SetFromMap) holds the map's keys.
CREATE PERFETTO TABLE _heap_graph_collection_element AS
SELECT r.id AS collection_id, e.element_id, e.slot_index, e.node_id, e.part
FROM _heap_graph_collection_row AS r
JOIN _heap_graph_collection_own_element AS e
  ON e.collection_id = r.count_id
WHERE
  r.kind = 'map'
  OR e.part IS NOT 'value';

CREATE PERFETTO INDEX _heap_graph_collection_element_idx ON _heap_graph_collection_element(
  element_id
);

CREATE PERFETTO INDEX _heap_graph_collection_element_collection_idx ON _heap_graph_collection_element(
  collection_id
);

-- Collections nested in another collection, under the collection whose
-- structure owns them, with the role they have in it ('key', 'value' or
-- NULL).
CREATE PERFETTO TABLE _heap_graph_collection_nesting AS
SELECT
  min(e.collection_id) AS source_node_id,
  e.element_id AS dest_node_id,
  min(e.part) AS part
FROM _heap_graph_collection_element AS e
JOIN _heap_graph_collection_row AS c
  ON c.id = e.element_id
JOIN _heap_graph_collection_parent AS d
  ON d.id = e.element_id
JOIN _heap_graph_collection_structure AS s
  ON s.id = d.parent_id
  AND s.collection_id = e.collection_id
GROUP BY
  e.element_id;

CREATE PERFETTO INDEX _heap_graph_collection_nesting_idx ON _heap_graph_collection_nesting(
  dest_node_id
);

-- The named reference holding every reported collection: preferably from its
-- immediate dominator, then from its shortest path parent.
CREATE PERFETTO TABLE _heap_graph_collection_direct_field AS
SELECT id, owner_id, field_name, field_type_name
FROM (
  SELECT
    r.owned_id AS id,
    r.owner_id,
    coalesce(r.deobfuscated_field_name, r.field_name) AS field_name,
    r.field_type_name,
    row_number() OVER (
      PARTITION BY
        r.owned_id
      ORDER BY
        r.owner_id = d.parent_id DESC,
        r.owner_id = m.parent_id DESC,
        r.id
    ) AS rank
  FROM heap_graph_reference AS r
  CROSS JOIN _heap_graph_collection_row AS c
    ON c.id = r.owned_id
  LEFT JOIN _heap_graph_collection_parent AS d
    ON d.id = r.owned_id
  LEFT JOIN _heap_graph_object_min_depth_tree AS m
    ON m.id = r.owned_id
  WHERE
    r.field_name != ''
    AND NOT (r.field_name GLOB '[[]*]')
)
WHERE
  rank = 1
ORDER BY
  id;

-- The field holding every reported collection. A collection nested in
-- another one gets that collection's field with its role appended:
-- 'Foo.mMap[value]', 'Foo.mList[]'.
CREATE PERFETTO TABLE _heap_graph_collection_owning_field AS
WITH
  top AS (
    SELECT c.id, d.field_name, d.field_type_name, d.owner_id
    FROM _heap_graph_collection_row AS c
    LEFT JOIN _heap_graph_collection_direct_field AS d USING (id)
    WHERE
      c.id NOT IN (SELECT dest_node_id FROM _heap_graph_collection_nesting)
  )
SELECT id, field_name, field_type_name, owner_id
FROM _graph_scan!(
  (SELECT source_node_id, dest_node_id FROM _heap_graph_collection_nesting),
  top,
  (field_name, field_type_name, owner_id),
  (
    SELECT
    t.id,
    t.field_name || "[" || ifnull(n.part, "") || "]" AS field_name,
    NULL AS field_type_name,
    t.owner_id
    FROM $table AS t
    JOIN _heap_graph_collection_nesting AS n
    ON n.dest_node_id = t.id
  )
);

CREATE PERFETTO INDEX _heap_graph_collection_owning_field_idx ON _heap_graph_collection_owning_field(
  id
);

-- The innermost reported collection owning or being every object, carried
-- down _heap_graph_collection_parent. NULL outside collections.
CREATE PERFETTO TABLE _heap_graph_collection_object_enclosing AS
SELECT id, enclosing_id
FROM _graph_scan!(
  (
    SELECT parent_id AS source_node_id, id AS dest_node_id
    FROM _heap_graph_collection_parent
    WHERE
    parent_id IS NOT NULL
  ),
  (
    SELECT p.id, r.id AS enclosing_id
    FROM _heap_graph_collection_parent AS p
    LEFT JOIN _heap_graph_collection_row AS r
    USING (id)
    WHERE
    p.parent_id IS NULL
  ),
  (enclosing_id),
  (
    SELECT t.id, ifnull(r.id, t.enclosing_id) AS enclosing_id
    FROM $table AS t
    LEFT JOIN _heap_graph_collection_row AS r
    USING (id)
  )
)
ORDER BY
  id;

CREATE PERFETTO INDEX _heap_graph_collection_object_enclosing_idx ON _heap_graph_collection_object_enclosing(
  id
);

-- The innermost collection owning every reported collection which is owned
-- by one, directly or through other objects (e.g. its elements).
CREATE PERFETTO TABLE _heap_graph_collection_outer AS
SELECT r.id, e.enclosing_id AS outer_id
FROM _heap_graph_collection_row AS r
JOIN _heap_graph_collection_parent AS p USING (id)
JOIN _heap_graph_collection_object_enclosing AS e
  ON e.id = p.parent_id
WHERE
  e.enclosing_id IS NOT NULL
ORDER BY
  r.id;

CREATE PERFETTO INDEX _heap_graph_collection_outer_idx ON _heap_graph_collection_outer(
  id
);

-- Pairs of reported collections with the same owning field and class where
-- one (`ancestor_id`) owns the other (`id`), directly or not: e.g. the child
-- arrays of nested ViewGroup.mChildren. The retained size of the ancestor
-- includes that of `id`.
CREATE PERFETTO TABLE _heap_graph_collection_nested_in_group AS
WITH
  ancestor AS (
    SELECT id, ancestor_id
    FROM _graph_scan!(
      (
        SELECT outer_id AS source_node_id, id AS dest_node_id
        FROM _heap_graph_collection_outer
      ),
      (
        SELECT DISTINCT outer_id AS id, outer_id AS ancestor_id
        FROM _heap_graph_collection_outer
      ),
      (ancestor_id),
      (SELECT id, ancestor_id FROM $table)
    )
  )
SELECT DISTINCT a.id, a.ancestor_id
FROM ancestor AS a
JOIN _heap_graph_collection_row AS c
  ON c.id = a.id
JOIN _heap_graph_collection_row AS ac
  ON ac.id = a.ancestor_id
JOIN _heap_graph_collection_owning_field AS f
  ON f.id = a.id
JOIN _heap_graph_collection_owning_field AS af
  ON af.id = a.ancestor_id
WHERE
  a.id != a.ancestor_id
  AND c.class_name = ac.class_name
  AND f.field_name IS af.field_name
ORDER BY
  a.id;

-- Element count, capacity and overhead of every reported collection.
CREATE PERFETTO TABLE _heap_graph_collection_metric AS
WITH
  -- Entries of every collection with its own storage, counted once on its
  -- unfiltered elements (a set over a map holding null has an entry whose only
  -- element is the value).
  own_count AS (
    SELECT
      e.collection_id AS id,
      count(e.element_id) AS elements,
      count(DISTINCT e.node_id) AS nodes,
      count(DISTINCT e.slot_index / 2) AS pairs
    FROM _heap_graph_collection_own_element AS e
    GROUP BY
      e.collection_id
  ),
  own_metric AS (
    SELECT
      c.id,
      CASE
        WHEN c.layout = 'nodes' THEN ifnull(n.nodes, 0)
        WHEN c.layout = 'slot_pairs' THEN ifnull(n.pairs, 0)
        WHEN c.layout = 'fields'
        AND c.kind = 'map' THEN min(ifnull(n.elements, 0), 1)
        ELSE ifnull(n.elements, 0)
      END AS element_count,
      iif(st.layout = 'slot_pairs', st.length / 2, st.length) AS capacity
    FROM _heap_graph_collection_object AS c
    LEFT JOIN own_count AS n USING (id)
    LEFT JOIN _heap_graph_collection_storage AS st
      ON st.collection_id = c.id
      AND st.layout != 'delegate'
    WHERE
      c.layout NOT IN ('delegate', 'unsupported')
    UNION ALL
    SELECT a.id, ifnull(n.elements, 0), a.length
    FROM _heap_graph_collection_raw_array AS a
    LEFT JOIN own_count AS n USING (id)
  ),
  overhead AS (
    SELECT s.collection_id AS id, sum(max(o.self_size, 0)) AS overhead_size
    FROM _heap_graph_collection_structure AS s
    JOIN heap_graph_object AS o USING (id)
    GROUP BY
      s.collection_id
  )
SELECT r.id, m.element_count, m.capacity, ov.overhead_size
FROM _heap_graph_collection_row AS r
LEFT JOIN own_metric AS m
  ON m.id = r.count_id
LEFT JOIN overhead AS ov
  ON ov.id = r.id
ORDER BY
  r.id;

CREATE PERFETTO INDEX _heap_graph_collection_metric_idx ON _heap_graph_collection_metric(
  id
);

-- The most common element class of every collection.
CREATE PERFETTO TABLE _heap_graph_collection_element_class AS
WITH
  class_count AS (
    SELECT e.collection_id, o.type_id, count() AS n
    FROM _heap_graph_collection_element AS e
    JOIN heap_graph_object AS o
      ON o.id = e.element_id
    GROUP BY
      1,
      2
  )
SELECT
  k.collection_id AS id,
  count() AS distinct_element_class_count,
  max(coalesce(c.deobfuscated_name, c.name)) FILTER (WHERE k.rank = 1) AS element_class
FROM (
  SELECT
    *,
    row_number() OVER (PARTITION BY collection_id ORDER BY n DESC, type_id) AS rank
  FROM class_count
) AS k
JOIN heap_graph_class AS c
  ON c.id = k.type_id
GROUP BY
  k.collection_id
ORDER BY
  k.collection_id;

CREATE PERFETTO INDEX _heap_graph_collection_element_class_idx ON _heap_graph_collection_element_class(
  id
);

-- The size of the objects every object owns, self inclusive: like
-- heap_graph_dominator_tree, over _heap_graph_collection_parent.
CREATE PERFETTO TABLE _heap_graph_collection_owned_size AS
SELECT c0 AS id, c3 AS size, c4 AS native_size
FROM __intrinsic_table_ptr(
  (
    SELECT
      __intrinsic_tree_dominator_summary(
        __intrinsic_tree_from_table(
          'id',
          p.id,
          'parent_id',
          p.parent_id,
          'self_size',
          max(o.self_size, 0),
          'native_size',
          o.native_size,
          'self_count',
          1
        )
      )
    FROM _heap_graph_collection_parent AS p
    JOIN heap_graph_object AS o USING (id)
  )
)
WHERE
  __intrinsic_table_ptr_bind(c0, 'id')
  AND __intrinsic_table_ptr_bind(c1, 'idom_id')
  AND __intrinsic_table_ptr_bind(c2, 'dominated_obj_count')
  AND __intrinsic_table_ptr_bind(c3, 'dominated_size_bytes')
  AND __intrinsic_table_ptr_bind(c4, 'dominated_native_size_bytes')
  AND __intrinsic_table_ptr_bind(c5, 'depth')
ORDER BY
  id;

-- Every collection in the heap graph.
--
-- `kind` is 'list', 'set', 'queue', 'map', 'collection' or 'array'. `layout`
-- is the recipe layout ('slots', 'slot_pairs', 'nodes', 'delegate',
-- 'fields'), 'array' for reference arrays or 'unsupported' for collections
-- without a recipe, which have no element count, capacity or overhead.
-- `element_count` is the number of non-null elements (entries for maps),
-- `capacity` the slots of the backing array (entries for array maps),
-- `overhead_size` the size of the collection and its internal objects,
-- `retained_size` the size of the objects it owns (see
-- _heap_graph_collection_parent) and `reachable_size` its cumulative size in
-- the shortest path tree. `field_name` is the field holding it and `owner_id`
-- the object holding it (see _heap_graph_collection_owning_field).
-- `element_class` is its most common element class.
CREATE PERFETTO TABLE _heap_graph_collection AS
SELECT
  r.id,
  r.upid,
  r.graph_sample_ts,
  r.kind,
  r.layout,
  r.class_name,
  r.recipe_class,
  n.element_count,
  n.capacity,
  iif(
    r.layout = 'unsupported',
    NULL,
    ifnull(n.overhead_size, max(o.self_size, 0))
  ) AS overhead_size,
  w.size AS retained_size,
  w.native_size AS retained_native_size,
  a.cumulative_size AS reachable_size,
  o.heap_type,
  f.field_name,
  f.field_type_name,
  f.owner_id,
  ec.element_class,
  iif(
    r.layout = 'unsupported',
    NULL,
    ifnull(ec.distinct_element_class_count, 0)
  ) AS distinct_element_class_count
FROM _heap_graph_collection_row AS r
JOIN heap_graph_object AS o
  ON o.id = r.id
LEFT JOIN _heap_graph_collection_metric AS n
  ON n.id = r.id
LEFT JOIN _heap_graph_collection_owned_size AS w
  ON w.id = r.id
LEFT JOIN _heap_graph_object_tree_aggregation AS a
  ON a.id = r.id
LEFT JOIN _heap_graph_collection_owning_field AS f
  ON f.id = r.id
LEFT JOIN _heap_graph_collection_element_class AS ec
  ON ec.id = r.id
ORDER BY
  r.id;

-- Summarizes `collections` (rows of _heap_graph_collection) per owning field
-- and class: one row per (heap graph, field_name, class_name), with the
-- distribution of element counts over the instances (p50 and p90 are
-- nearest-rank), summed sizes and the most common element class. The
-- retained and reachable sums leave out instances owned by another instance
-- of the same row (see _heap_graph_collection_nested_in_group), whose sizes
-- the outer one already includes.
CREATE PERFETTO MACRO _heap_graph_collection_summary(
  collections TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  WITH
    ranked AS (
      SELECT
        *,
        row_number() OVER (
          PARTITION BY
            upid,
            graph_sample_ts,
            field_name,
            class_name
          ORDER BY element_count IS NULL, element_count
        ) AS rank,
        count(element_count) OVER (PARTITION BY upid, graph_sample_ts, field_name, class_name) AS n,
        id NOT IN (
          SELECT g.id
          FROM _heap_graph_collection_nested_in_group AS g
          WHERE
            g.ancestor_id IN (
              SELECT id
              FROM $collections
            )
        ) AS outermost
      FROM $collections
    ),
    summary AS (
      SELECT
        upid,
        graph_sample_ts,
        field_name,
        class_name,
        min(kind) AS kind,
        min(layout) AS layout,
        count() AS instance_count,
        sum(element_count) AS element_count_sum,
        max(element_count) FILTER (WHERE rank = (n + 1) / 2) AS element_count_p50,
        max(element_count) FILTER (WHERE rank = (9 * n + 9) / 10) AS element_count_p90,
        max(element_count) AS element_count_max,
        sum(capacity) AS capacity_sum,
        max(capacity) AS capacity_max,
        sum(overhead_size) AS overhead_size_sum,
        sum(retained_size) FILTER (WHERE outermost) AS retained_size_sum,
        sum(reachable_size) FILTER (WHERE outermost) AS reachable_size_sum
      FROM ranked
      GROUP BY
        upid,
        graph_sample_ts,
        field_name,
        class_name
    ),
    element_class AS (
      SELECT
        upid,
        graph_sample_ts,
        field_name,
        class_name,
        element_class,
        row_number() OVER (
          PARTITION BY
            upid,
            graph_sample_ts,
            field_name,
            class_name
          ORDER BY count() DESC, element_class
        ) AS rank
      FROM $collections
      WHERE
        element_class IS NOT NULL
      GROUP BY
        upid,
        graph_sample_ts,
        field_name,
        class_name,
        element_class
    )
  SELECT s.*, e.element_class
  FROM summary AS s
  LEFT JOIN element_class AS e
    ON e.upid = s.upid
    AND e.graph_sample_ts = s.graph_sample_ts
    AND e.field_name IS s.field_name
    AND e.class_name = s.class_name
    AND e.rank = 1
);

-- _heap_graph_collection summarized per owning field and class.
CREATE PERFETTO TABLE _heap_graph_collection_field_summary AS
SELECT * FROM _heap_graph_collection_summary!(_heap_graph_collection);

-- Collection classes without a recipe, per heap graph, with the reference
-- fields of an instance ('Declaring.field:declared_type'): what a recipe for
-- them needs.
CREATE PERFETTO TABLE _heap_graph_collection_unsupported_class AS
WITH
  per_class AS (
    SELECT
      upid,
      graph_sample_ts,
      class_name,
      count() AS instance_count,
      sum(retained_size) AS retained_size_sum,
      sum(reachable_size) AS reachable_size_sum,
      min(id) AS sample_id
    FROM _heap_graph_collection
    WHERE
      layout = 'unsupported'
    GROUP BY
      upid,
      graph_sample_ts,
      class_name
  )
SELECT
  p.upid,
  p.graph_sample_ts,
  p.class_name,
  p.instance_count,
  p.retained_size_sum,
  p.reachable_size_sum,
  group_concat(DISTINCT r.field_name || ':' || r.field_type_name) AS reference_fields
FROM per_class AS p
JOIN heap_graph_object AS o
  ON o.id = p.sample_id
LEFT JOIN heap_graph_reference AS r
  ON r.reference_set_id = o.reference_set_id
GROUP BY
  p.sample_id
ORDER BY
  p.reachable_size_sum DESC;
