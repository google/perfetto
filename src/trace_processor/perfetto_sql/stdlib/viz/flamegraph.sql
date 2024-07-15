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

CREATE PERFETTO MACRO _viz_flamegraph_filter_and_hash(
  tab TableOrSubquery,
  show_from_frame_bits Expr
)
RETURNS TableOrSubquery
AS (
  SELECT id, hash, parentHash, depth, showFromFrameBits, stackBits
  FROM _graph_scan!(
    (
      SELECT parentId AS source_node_id, id AS dest_node_id
      FROM $tab
      WHERE parentId IS NOT NULL
    ),
    (
      select
        id,
        IIF(showFrame AND showFromFrameBits = $show_from_frame_bits, HASH(name), 0) AS hash,
        IIF(showFrame AND showFromFrameBits = $show_from_frame_bits, 0, NULL) AS parentHash,
        IIF(showFrame AND showFromFrameBits = $show_from_frame_bits, 0, -1) AS depth,
        IIF(showFrame AND showFromFrameBits = $show_from_frame_bits, showFromFrameBits, 0) AS showFromFrameBits,
        IIF(showFrame AND showFromFrameBits = $show_from_frame_bits, stackBits, 0) AS stackBits
      FROM $tab
      WHERE parentId IS NULL
    ),
    (hash, parentHash, depth, showFromFrameBits, stackBits),
    (
      select
        t.id as id,
        IIF(
          x.showFrame AND (t.showFromFrameBits | x.showFromFrameBits) = $show_from_frame_bits,
          HASH(t.hash, name),
          t.hash
        ) AS hash,
        IIF(
          x.showFrame AND (t.showFromFrameBits | x.showFromFrameBits) = $show_from_frame_bits,
          t.hash,
          t.parentHash
        ) AS parentHash,
        IIF(
          x.showFrame AND (t.showFromFrameBits | x.showFromFrameBits) = $show_from_frame_bits,
          t.depth + 1,
          t.depth
        ) AS depth,
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
      JOIN $tab x USING (id)
    )
  ) g
  WHERE parentHash IS NOT NULL
  ORDER BY hash
);

CREATE PERFETTO MACRO _viz_flamegraph_merge_hashes(
  tab TableOrSubquery,
  source TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SELECT
    c._auto_id AS id,
    (
      SELECT p._auto_id
      FROM $tab p
      WHERE p.hash = c.parentHash
      LIMIT 1
    ) AS parentId,
    c.depth,
    c.stackBits,
    s.name,
    SUM(s.value) AS value
  FROM $tab c
  JOIN $source s USING (id)
  GROUP BY hash
);

CREATE PERFETTO MACRO _viz_flamegraph_accumulate(
  tab TableOrSubquery,
  showStackBits Expr
)
RETURNS TableOrSubquery
AS (
  SELECT id, cumulativeValue
  FROM _graph_scan!(
    (
      SELECT id AS source_node_id, parentId AS dest_node_id
      FROM $tab
      WHERE parentId IS NOT NULL
    ),
    (
      SELECT t.id AS id, t.value AS cumulativeValue
      FROM $tab t
      LEFT JOIN $tab c ON t.id = c.parentId
      WHERE c.id IS NULL AND t.stackBits = $showStackBits
    ),
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
        GROUP BY 1
      ) x
      JOIN $tab t USING (id)
    )
  )
  ORDER BY id
);

CREATE PERFETTO MACRO _viz_flamegraph_local_layout(
  acc TableOrSubquery,
  tab TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SELECT id, xEnd - cumulativeValue as xStart, xEnd
  FROM (
    SELECT
      b.id,
      b.cumulativeValue,
      SUM(b.cumulativeValue) OVER win AS xEnd
    FROM $acc b
    JOIN $tab s USING (id)
    WHERE b.cumulativeValue > 0
    WINDOW win AS (
      PARTITION BY s.parentId
      ORDER BY b.cumulativeValue DESC
      ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
    )
  )
  ORDER BY id
);

CREATE PERFETTO MACRO _viz_flamegraph_global_layout(
  layout TableOrSubquery,
  acc TableOrSubquery,
  tab TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SELECT
    s.id,
    IFNULL(t.parentId, -1) AS parentId,
    t.depth,
    IIF(t.name = '', 'unknown', t.name) AS name,
    t.value AS selfValue,
    b.cumulativeValue,
    s.xStart,
    s.xEnd
  FROM _graph_scan!(
    (
      SELECT parentId AS source_node_id, id AS dest_node_id
      FROM $tab
      WHERE parentId IS NOT NULL
    ),
    (
      SELECT b.id AS id, w.xStart, w.xEnd
      FROM $acc b
      JOIN $tab t USING (id)
      JOIN $layout w USING (id)
      WHERE t.parentId IS NULL
    ),
    (xStart, xEnd),
    (
      SELECT
        t.id,
        t.xStart + w.xStart AS xStart,
        t.xStart + w.xEnd AS xEnd
      FROM $table t
      JOIN $layout w USING (id)
    )
  ) s
  JOIN $tab t USING (id)
  JOIN $acc b USING (id)
  ORDER BY depth, xStart
);
