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

-- Adds presentation layout for one of the flamegraph's configured values.
-- Passing a different cumulative column lays out the same structural tree for
-- that value without rebuilding the flamegraph.
CREATE PERFETTO MACRO _flamegraph_layout(
  tab TableOrSubquery,
  cumulative ColumnName
)
RETURNS TableOrSubquery
AS (
  WITH RECURSIVE
  source AS MATERIALIZED (
    SELECT * FROM $tab
  ),
  local_layout AS (
    SELECT
      _tree_id,
      $cumulative AS cumulativeValue,
      SUM($cumulative) OVER (
        PARTITION BY _tree_parent_id, depth
        ORDER BY $cumulative DESC, _tree_id
        ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
      ) - $cumulative AS xStart
    FROM source
    WHERE $cumulative > 0
  ),
  global_layout(_tree_id, rootDistance, xStart) AS (
    SELECT s._tree_id, 1, l.xStart
    FROM source s
    JOIN local_layout l USING (_tree_id)
    WHERE s._tree_parent_id IS NULL
    UNION ALL
    SELECT child._tree_id, parent.rootDistance + 1,
           parent.xStart + child_layout.xStart
    FROM global_layout parent
    JOIN source child ON child._tree_parent_id = parent._tree_id
    JOIN local_layout child_layout
      ON child_layout._tree_id = child._tree_id
  )
  SELECT
    s.*,
    parent_layout.cumulativeValue AS parentCumulativeValue,
    g.xStart,
    g.xStart + layout.cumulativeValue AS xEnd
  FROM global_layout g
  JOIN source s USING (_tree_id)
  JOIN local_layout layout USING (_tree_id)
  LEFT JOIN local_layout parent_layout
    ON parent_layout._tree_id = s._tree_parent_id
  ORDER BY g.rootDistance, g.xStart, s.depth DESC
);
