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

include perfetto module graphs.scan;

CREATE PERFETTO MACRO _viz_flamegraph_prepare_filter(
  tab TableOrSubquery,
  show_stack Expr,
  hide_stack Expr,
  show_from_frame Expr,
  hide_frame Expr,
  impossible_stack_bits Expr
)
RETURNS TableOrSubquery
AS (
  SELECT
    *,
    IIF($hide_stack, $impossible_stack_bits, $show_stack) AS stackBits,
    $show_from_frame As showFromFrameBits,
    $hide_frame = 0 AS showFrame
  FROM $tab
  ORDER BY id
);

CREATE PERFETTO MACRO _viz_flamegraph_filter_frames(
  source TableOrSubquery,
  show_from_frame_bits Expr
)
RETURNS TableOrSubquery
AS (
  WITH edges AS (
    SELECT parentId AS source_node_id, id AS dest_node_id
    FROM $source
    WHERE parentId IS NOT NULL
  ),
  inits AS (
    SELECT
      id,
      IIF(
        showFrame AND showFromFrameBits = $show_from_frame_bits,
        id,
        NULL
      ) AS filteredId,
      NULL AS filteredParentId,
      IIF(
        showFrame,
        showFromFrameBits,
        0
      ) AS showFromFrameBits,
      IIF(
        showFrame AND showFromFrameBits = $show_from_frame_bits,
        stackBits,
        0
      ) AS stackBits
    FROM $source
    WHERE parentId IS NULL
  )
  SELECT
    g.filteredId AS id,
    g.filteredParentId AS parentId,
    g.stackBits,
    SUM(t.value) AS value
  FROM _graph_scan!(
    edges,
    inits,
    (filteredId, filteredParentId, showFromFrameBits, stackBits),
    (
      SELECT
        t.id,
        IIF(
          x.showFrame AND (t.showFromFrameBits | x.showFromFrameBits) = $show_from_frame_bits,
          t.id,
          t.filteredId
        ) AS filteredId,
        IIF(
          x.showFrame AND (t.showFromFrameBits | x.showFromFrameBits) = $show_from_frame_bits,
          t.filteredId,
          t.filteredParentId
        ) AS filteredParentId,
        IIF(
          x.showFrame,
          (t.showFromFrameBits | x.showFromFrameBits),
          t.showFromFrameBits
        ) AS showFromFrameBits,
        IIF(
          x.showFrame AND (t.showFromFrameBits | x.showFromFrameBits) = $show_from_frame_bits,
          (t.stackBits | x.stackBits),
          t.stackBits
        ) AS stackBits
      FROM $table t
      JOIN $source x USING (id)
    )
  ) g
  JOIN $source t USING (id)
  WHERE filteredId IS NOT NULL
  GROUP BY filteredId
  ORDER BY filteredId
);

CREATE PERFETTO MACRO _viz_flamegraph_accumulate(
  filtered TableOrSubquery,
  showStackBits Expr
)
RETURNS TableOrSubquery
AS (
  WITH edges AS (
    SELECT id AS source_node_id, parentId AS dest_node_id
    FROM $filtered
    WHERE parentId IS NOT NULL
  ), inits AS (
    SELECT f.id, f.value AS cumulativeValue
    FROM $filtered f
    LEFT JOIN $filtered c ON c.parentId = f.id
    WHERE c.id IS NULL AND f.stackBits = $showStackBits
  )
  SELECT id, cumulativeValue
  FROM _graph_aggregating_scan!(
    edges,
    inits,
    (cumulativeValue),
    (
      SELECT
        x.id,
        x.childValue + IIF(
          t.stackBits = $showStackBits,
          t.value,
          0
        ) AS cumulativeValue
      FROM (
        SELECT id, SUM(cumulativeValue) AS childValue
        FROM $table
        GROUP BY id
      ) x
      JOIN $filtered t USING (id)
    )
  )
  ORDER BY id
);

CREATE PERFETTO MACRO _viz_flamegraph_hash(
  source TableOrSubquery,
  filtered TableOrSubquery,
  accumulated TableOrSubquery,
  show_from_frame_bits Expr
)
RETURNS TableOrSubquery
AS (
  SELECT
    g.id,
    g.hash,
    g.parentHash,
    g.depth,
    s.name,
    f.value,
    a.cumulativeValue
  FROM _graph_scan!(
    (
      SELECT parentId AS source_node_id, id AS dest_node_id
      FROM $filtered
      WHERE parentId IS NOT NULL
    ),
    (
      SELECT f.id, HASH(s.name) AS hash, NULL AS parentHash, 0 AS depth
      FROM $filtered f
      JOIN $source s USING (id)
      WHERE f.parentId IS NULL
    ),
    (hash, parentHash, depth),
    (
      SELECT
        t.id,
        HASH(t.hash, x.name) AS hash,
        t.hash AS parentHash,
        t.depth + 1 AS depth
      FROM $table t
      JOIN $source x USING (id)
    )
  ) g
  JOIN $source s USING (id)
  JOIN $filtered f USING (id)
  JOIN $accumulated a USING (id)
  ORDER BY hash
);

CREATE PERFETTO MACRO _viz_flamegraph_merge_hashes(
  hashed TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SELECT
    _auto_id AS id,
    (
      SELECT p._auto_id
      FROM $hashed p
      WHERE p.hash = c.parentHash
      LIMIT 1
    ) AS parentId,
    depth,
    name,
    SUM(value) AS value,
    SUM(cumulativeValue) AS cumulativeValue
  FROM $hashed c
  GROUP BY hash
);

CREATE PERFETTO MACRO _viz_flamegraph_local_layout(
  merged TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  WITH partial_layout AS (
    SELECT
      id,
      cumulativeValue,
      SUM(cumulativeValue) OVER win AS xEnd
    FROM $merged
    WHERE cumulativeValue > 0
    WINDOW win AS (
      PARTITION BY parentId
      ORDER BY cumulativeValue DESC
      ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
    )
  )
  SELECT id, xEnd - cumulativeValue as xStart, xEnd
  FROM partial_layout
  ORDER BY id
);

CREATE PERFETTO MACRO _viz_flamegraph_global_layout(
  merged TableOrSubquery,
  layout TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  WITH edges AS (
    SELECT parentId AS source_node_id, id AS dest_node_id
    FROM $merged
    WHERE parentId IS NOT NULL
  ),
  inits AS (
    SELECT h.id, l.xStart, l.xEnd
    FROM $merged h
    JOIN $layout l USING (id)
    WHERE h.parentId IS NULL
  )
  SELECT
    h.id,
    IFNULL(h.parentId, -1) AS parentId,
    IIF(h.name = '', 'unknown', h.name) AS name,
    h.value AS selfValue,
    h.cumulativeValue,
    h.depth,
    g.xStart,
    g.xEnd
  FROM _graph_scan!(
    edges,
    inits,
    (xStart, xEnd),
    (
      SELECT
        t.id,
        t.xStart + w.xStart AS xStart,
        t.xStart + w.xEnd AS xEnd
      FROM $table t
      JOIN $layout w USING (id)
    )
  ) g
  JOIN $merged h USING (id)
  ORDER BY depth, xStart
);
