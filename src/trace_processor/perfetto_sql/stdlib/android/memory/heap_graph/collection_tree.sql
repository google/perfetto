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
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- A tree of the paths from the heap roots to the collections, for flamegraphs:
-- the ownership tree of android.memory.heap_graph.collections aggregated by
-- class path, like _heap_graph_dominator_class_tree, with the collections as
-- leaves ('mListeners: java.util.ArrayList<Listener>'). A collection owned by
-- another collection is part of the outer one's leaf.

INCLUDE PERFETTO MODULE android.memory.heap_graph.collections;

INCLUDE PERFETTO MODULE android.memory.heap_graph.helpers;

INCLUDE PERFETTO MODULE graphs.scan;

-- 'c' for 'a.b.c'.
CREATE PERFETTO MACRO _heap_graph_last_name_segment(s Expr)
RETURNS Expr
AS substr($s, length(rtrim($s, replace($s, '.', ''))) + 1);

-- The frame name of every collection: 'field: Class<Element>'. The name hash
-- is salted so that a collection frame never merges with a class frame of the
-- same name.
CREATE PERFETTO TABLE _heap_graph_collection_label AS
SELECT id, name, HASH('collection', name) AS name_hash
FROM (
  SELECT
    id,
    iif(
      field_name IS NULL,
      '',
      _heap_graph_last_name_segment!(field_name) || ': '
    )
    || class_name
    || iif(
      element_class IS NULL
      OR kind = 'array',
      '',
      '<' || _heap_graph_last_name_segment!(element_class) || '>'
    ) AS name
  FROM _heap_graph_collection
)
ORDER BY
  id;

CREATE PERFETTO INDEX _heap_graph_collection_label_idx ON _heap_graph_collection_label(
  id
);

-- The path hash of every object of the ownership tree, and the collection
-- leaf it belongs to, if any. Within a leaf every object has the leaf's path.
-- (The step query is stringified by _graph_scan: it cannot hold single quoted
-- strings.)
CREATE PERFETTO TABLE _heap_graph_collection_object_path AS
SELECT id, path_hash, parent_path_hash, leaf_id
FROM _graph_scan!(
  (
    SELECT parent_id AS source_node_id, id AS dest_node_id
    FROM _heap_graph_collection_parent
    WHERE
    parent_id IS NOT NULL
  ),
  (
    SELECT
    p.id,
    ifnull(l.name_hash, k.name_hash) AS name_hash,
    HASH(
      o.upid,
      o.graph_sample_ts,
      ifnull(l.name_hash, k.name_hash),
      ifnull(o.root_type, ''),
      ifnull(o.heap_type, '')
    ) AS path_hash,
    0 AS parent_path_hash,
    l.id AS leaf_id
    FROM _heap_graph_collection_parent AS p
    JOIN heap_graph_object AS o
    USING (id)
    JOIN _heap_graph_class_name_hash AS k
    ON k.id = o.type_id
    LEFT JOIN _heap_graph_collection_label AS l
    USING (id)
    WHERE
    p.parent_id IS NULL
  ),
  (name_hash, path_hash, parent_path_hash, leaf_id),
  (
    SELECT
    t.id,
    iif(t.leaf_id IS NULL, ifnull(l.name_hash, k.name_hash), t.name_hash) AS name_hash,
    CASE
    WHEN t.leaf_id IS NOT NULL THEN t.path_hash
    WHEN ifnull(l.name_hash, k.name_hash) = t.name_hash THEN t.path_hash
    ELSE HASH(t.path_hash, ifnull(l.name_hash, k.name_hash), ifnull(o.heap_type, ""))
    END AS path_hash,
    iif(
      t.leaf_id IS NOT NULL OR ifnull(l.name_hash, k.name_hash) = t.name_hash,
      t.parent_path_hash,
      t.path_hash
    ) AS parent_path_hash,
    ifnull(t.leaf_id, l.id) AS leaf_id
    FROM $table AS t
    JOIN heap_graph_object AS o
    USING (id)
    JOIN _heap_graph_class_name_hash AS k
    ON k.id = o.type_id
    LEFT JOIN _heap_graph_collection_label AS l
    USING (id)
  )
)
ORDER BY
  id;

CREATE PERFETTO INDEX _heap_graph_collection_object_path_idx ON _heap_graph_collection_object_path(
  id
);

-- The frames: the distinct paths of objects outside leaves, and the leaves.
CREATE PERFETTO TABLE _heap_graph_collection_frame AS
WITH
  frame AS (
    SELECT
      path_hash,
      min(parent_path_hash) AS parent_path_hash,
      max(id IS leaf_id) AS is_leaf,
      coalesce(min(id) FILTER (WHERE id = leaf_id), min(id)) AS sample_id
    FROM _heap_graph_collection_object_path
    WHERE
      leaf_id IS NULL
      OR id = leaf_id
    GROUP BY
      path_hash
  )
SELECT
  row_number() OVER (ORDER BY path_hash) AS id,
  path_hash,
  parent_path_hash,
  is_leaf,
  sample_id
FROM frame
ORDER BY
  id;

CREATE PERFETTO INDEX _heap_graph_collection_frame_idx ON _heap_graph_collection_frame(
  path_hash
);

-- Every leaf frame with itself and each frame above it.
CREATE PERFETTO TABLE _heap_graph_collection_frame_ancestor AS
SELECT DISTINCT leaf_frame_id, id AS frame_id
FROM _graph_scan!(
  (
    SELECT f.id AS source_node_id, p.id AS dest_node_id
    FROM _heap_graph_collection_frame AS f
    JOIN _heap_graph_collection_frame AS p
    ON p.path_hash = f.parent_path_hash
  ),
  (SELECT id, id AS leaf_frame_id FROM _heap_graph_collection_frame WHERE is_leaf),
  (leaf_frame_id),
  (SELECT DISTINCT id, leaf_frame_id FROM $table)
)
ORDER BY
  frame_id;

CREATE PERFETTO INDEX _heap_graph_collection_frame_ancestor_idx ON _heap_graph_collection_frame_ancestor(
  frame_id
);

-- The leaf frame of every collection: its own (`is_leaf`), or that of the
-- collection owning it.
CREATE PERFETTO TABLE _heap_graph_collection_leaf_frame AS
SELECT c.id AS collection_id, f.id AS frame_id, p.leaf_id = c.id AS is_leaf
FROM _heap_graph_collection AS c
JOIN _heap_graph_collection_object_path AS p USING (id)
JOIN _heap_graph_collection_frame AS f
  ON f.path_hash = p.path_hash
ORDER BY
  frame_id;

CREATE PERFETTO INDEX _heap_graph_collection_leaf_frame_idx ON _heap_graph_collection_leaf_frame(
  frame_id
);

-- The collections of every leaf frame: count, retained size and elements.
CREATE PERFETTO TABLE _heap_graph_collection_frame_value AS
SELECT
  l.frame_id,
  count() AS self_count,
  sum(c.retained_size) AS self_size,
  ifnull(sum(c.element_count), 0) AS self_element_count
FROM _heap_graph_collection AS c
JOIN _heap_graph_collection_leaf_frame AS l
  ON l.collection_id = c.id
WHERE
  l.is_leaf
GROUP BY
  l.frame_id
ORDER BY
  l.frame_id;

-- The collections tree: the frames from the roots to the collections. Only
-- leaves have values: `self_count` collections, `self_size` their retained
-- size and `self_element_count` their elements. `collection_class` is set on
-- leaf frames. `path_hash_stable` identifies a frame for
-- _heap_graph_collections_under_frames!.
CREATE PERFETTO TABLE _heap_graph_collection_tree AS
SELECT
  f.id,
  p.id AS parent_id,
  o.upid,
  o.graph_sample_ts,
  coalesce(l.name, k.deobfuscated_name, k.name, '') AS name,
  iif(f.is_leaf, coalesce(k.deobfuscated_name, k.name, ''), NULL) AS collection_class,
  o.root_type,
  o.heap_type,
  ifnull(v.self_count, 0) AS self_count,
  ifnull(v.self_size, 0) AS self_size,
  ifnull(v.self_element_count, 0) AS self_element_count,
  f.path_hash AS path_hash_stable
FROM _heap_graph_collection_frame AS f
JOIN heap_graph_object AS o
  ON o.id = f.sample_id
JOIN heap_graph_class AS k
  ON k.id = o.type_id
LEFT JOIN _heap_graph_collection_label AS l
  ON l.id = f.sample_id
LEFT JOIN _heap_graph_collection_frame_value AS v
  ON v.frame_id = f.id
LEFT JOIN _heap_graph_collection_frame AS p
  ON p.path_hash = f.parent_path_hash
WHERE
  f.id IN (SELECT frame_id FROM _heap_graph_collection_frame_ancestor)
ORDER BY
  f.id;

-- The ids of the collections below the frames of _heap_graph_collection_tree
-- whose `path_hash_stable` is in `path_hashes` (a subquery with a
-- `path_hash` column). `is_leaf` is 1 for the collections that are leaf frames
-- themselves and 0 for those nested in one.
CREATE PERFETTO MACRO _heap_graph_collections_under_frames(
  path_hashes TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SELECT DISTINCT l.collection_id AS id, l.is_leaf
  FROM _heap_graph_collection_frame AS f
  JOIN _heap_graph_collection_frame_ancestor AS a
    ON a.frame_id = f.id
  JOIN _heap_graph_collection_leaf_frame AS l
    ON l.frame_id = a.leaf_frame_id
  WHERE
    f.path_hash IN (SELECT path_hash FROM $path_hashes)
);
