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

-- sqlformat file off

-- Computes the flame-chart rectangle set ("runs") for a stack tree and a
-- series of sample points.
--
-- For each stack depth, a run spans the time range over which the same frame
-- was continuously present at that depth: consecutive points sharing a stack
-- prefix extend the shared runs, only divergent depths open new ones. Runs
-- still open after the last point are closed at its timestamp, so a run seen
-- in a single point has zero duration.
--
-- Example usage:
-- ```
-- SELECT ts, dur, depth, id, sample_count
-- FROM _flamechart_runs!(
--   _tree_from_table!(
--     (SELECT id, parent_id, name FROM stacks),
--     (name)
--   ),
--   (SELECT ts, callsite_id AS leaf_id FROM samples ORDER BY ts)
-- );
-- ```
CREATE PERFETTO MACRO _flamechart_runs(
  -- A TREE pointer from _tree_from_table! encoding the stack structure.
  tree Expr,
  -- A table/view/subquery of sample points with columns 'ts' and 'leaf_id'.
  -- Rows must be ordered by ts. 'leaf_id' references the tree's id column
  -- and names the innermost (leaf) frame of the sample's stack; points with
  -- a null or unresolvable leaf_id are skipped.
  points TableOrSubquery
)
-- Returns the runs: ts, dur, depth (0 = outermost frame), id (the id of the
-- frame's node in the tree's source table) and sample_count (number of
-- points in the run).
RETURNS TableOrSubquery
AS (
  SELECT
    c0 AS ts,
    c1 AS dur,
    c2 AS depth,
    c3 AS id,
    c4 AS sample_count
  FROM __intrinsic_table_ptr(
    (SELECT __intrinsic_flamechart($tree, p.ts, p.leaf_id) FROM $points AS p)
  )
  WHERE
    __intrinsic_table_ptr_bind(c0, 'ts')
    AND __intrinsic_table_ptr_bind(c1, 'dur')
    AND __intrinsic_table_ptr_bind(c2, 'depth')
    AND __intrinsic_table_ptr_bind(c3, 'id')
    AND __intrinsic_table_ptr_bind(c4, 'sample_count')
);
