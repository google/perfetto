--
-- Copyright 2024 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the 'License');
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an 'AS IS' BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

CREATE PERFETTO MACRO _perf_callsites_for_samples(samples TableOrSubquery)
RETURNS TableOrSubquery
AS
(
  WITH cs AS MATERIALIZED (
    SELECT callsite_id, COUNT() cnt FROM $samples GROUP BY 1
  )
  SELECT
    c.id,
    c.parent_id AS parentId,
    COALESCE(f.deobfuscated_name, f.name, '[unknown]') AS name,
    IFNULL((SELECT cnt FROM cs WHERE cs.callsite_id = c.id), 0) AS self_count
  FROM graph_reachable_dfs!(
    (
        SELECT
          c.id AS source_node_id,
          c.parent_id AS dest_node_id
        FROM stack_profile_callsite c
    ),
    (SELECT callsite_id AS node_id FROM cs)
  ) g
  JOIN stack_profile_callsite c ON c.id = g.node_id
  JOIN stack_profile_frame f ON c.frame_id = f.id
);
