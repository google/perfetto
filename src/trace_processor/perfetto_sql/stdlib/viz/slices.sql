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

-- sqlformat file off
-- This file is case-sensitive.

INCLUDE PERFETTO MODULE slices.with_context;

-- The aggregating scan computed each node's self_dur as its own dur minus the
-- sum of its children's dur. Here, `self_dur` is just a per-node arithmetic
-- expression over the direct children, so the recursive scan reduces to a
-- single-hop aggregate over the parent edge with no tree operator needed.
CREATE PERFETTO MACRO _viz_slice_ancestor_agg(
  inits TableOrSubquery,
  nodes TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SUBPIPELINE child_dur AS (
    FROM $nodes AS c
    |> WHERE c.parent_id IS NOT NULL
    |> AGGREGATE sum(c.dur) AS child_dur GROUP BY c.parent_id
    |> SELECT parent_id AS id, child_dur
  )
  FROM $nodes AS s
  |> LEFT JOIN child_dur AS cd USING (id)
  |> SELECT
       s.id,
       s.parent_id AS parentId,
       s.name,
       s.dur - coalesce(cd.child_dur, 0) AS self_dur,
       1 AS self_count,
       1 AS simple_count
);

CREATE PERFETTO PIPELINE _viz_slices_for_ui_table AS
FROM thread_or_process_slice
|> UNION ALL (
     FROM slice
     |> JOIN track ON slice.track_id = track.id
     |> WHERE
          NOT (slice.track_id IN (SELECT id FROM process_track))
          AND NOT (slice.track_id IN (SELECT id FROM thread_track))
     |> SELECT
          slice.id,
          slice.ts,
          slice.dur,
          slice.category,
          slice.name,
          slice.track_id,
          track.name AS track_name,
          NULL AS thread_name,
          NULL AS utid,
          NULL AS tid,
          NULL AS process_name,
          NULL AS upid,
          NULL AS pid,
          slice.depth,
          slice.parent_id,
          slice.arg_set_id
   );
