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

-- Lays out a merged flamegraph tree for rendering.
--
-- The input is a flat merged tree as produced by the flamegraph
-- intrinsics: rows in render order (depth first, widest sibling first,
-- ids equal to row numbers) with columns id, depth (positive going down,
-- negative going up), cumulativeValue, matchedSelfValue and
-- ancestorMatchedSelfValue; any other columns are passed through.
--
-- The output adds xStart and xEnd, the node's horizontal extent: in
-- render order, everything drawn left of a node is the complete subtrees
-- preceding it minus what its own ancestors span, so xStart is a prefix
-- sum of matchedSelfValue with the ancestors' share subtracted. No
-- recursion is needed. Rows come out ordered by distance from the root
-- then xStart, with the downward tree first on ties.
CREATE PERFETTO MACRO _flamegraph_layout(tab TableOrSubquery)
RETURNS TableOrSubquery AS
(
  SELECT
    *,
    xStart + cumulativeValue AS xEnd
  FROM (
    SELECT
      *,
      COALESCE(
        SUM(matchedSelfValue) OVER (
          PARTITION BY depth < 0
          ORDER BY id
          ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING),
        0) - ancestorMatchedSelfValue AS xStart
    FROM $tab
  )
  ORDER BY abs(depth), xStart, depth DESC
)
