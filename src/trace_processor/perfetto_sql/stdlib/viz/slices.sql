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

INCLUDE PERFETTO MODULE graphs.scan;

CREATE PERFETTO MACRO _viz_slice_ancestor_agg(
  inits TableOrSubquery
)
RETURNS TableOrSubquery
AS
(
  SELECT id, parent_id AS parentId, name, self_dur, self_count
  FROM _graph_aggregating_scan!(
    (
      SELECT id AS source_node_id, parent_id AS dest_node_id
      FROM slice
      WHERE parent_id IS NOT NULL
    ),
    (SELECT id, dur, dur AS self_dur, 1 AS self_count FROM $inits),
    (dur, self_dur, self_count),
    (
      WITH agg AS (
        SELECT t.id, sum(t.dur) AS child_dur
        FROM $table t
        GROUP BY id
      )
      SELECT a.id, s.dur, s.dur - a.child_dur AS self_dur, 0 AS self_count
      FROM agg a
      JOIN slice s USING (id)
    )
  ) g
  JOIN slice s USING (id)
);
