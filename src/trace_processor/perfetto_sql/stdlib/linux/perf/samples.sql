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

INCLUDE PERFETTO MODULE callstacks.stack_profile;
INCLUDE PERFETTO MODULE graphs.scan;

CREATE PERFETTO MACRO _linux_perf_callstacks_for_samples(
  samples TableOrSubquery
)
RETURNS TableOrSubquery
AS
(
  WITH metrics AS MATERIALIZED (
    SELECT
      callsite_id,
      COUNT() AS self_count
    FROM $samples
    GROUP BY callsite_id
  )
  SELECT
    c.id,
    c.parent_id,
    c.name,
    c.mapping_name,
    c.source_file,
    c.line_number,
    IFNULL(m.self_count, 0) AS self_count
  FROM _callstacks_for_stack_profile_samples!(metrics) c
  LEFT JOIN metrics m USING (callsite_id)
);

CREATE PERFETTO TABLE _linux_perf_raw_callstacks AS
SELECT
  c.id,
  c.parent_id,
  c.name,
  c.mapping_name,
  c.source_file,
  c.line_number,
  c.self_count AS self_count
FROM _linux_perf_callstacks_for_samples!(
  (SELECT p.callsite_id FROM perf_sample p)
) c
ORDER BY c.id;

-- Table summarising the callstacks captured during all
-- perf samples in the trace.
--
-- Specifically, this table returns a tree containing all
-- the callstacks seen during the trace with `self_count`
-- equal to the number of samples with that frame as the
-- leaf and `cumulative_count` equal to the number of
-- samples with the frame anywhere in the tree.
CREATE PERFETTO TABLE linux_perf_samples_summary_tree(
  -- The id of the callstack. A callstack in this context
  -- is a unique set of frames up to the root.
  id INT,
  -- The id of the parent callstack for this callstack.
  parent_id INT,
  -- The function name of the frame for this callstack.
  name STRING,
  -- The name of the mapping containing the frame. This
  -- can be a native binary, library, JAR or APK.
  mapping_name STRING,
  -- The name of the file containing the function.
  source_file STRING,
  -- The line number in the file the function is located at.
  line_number INT,
  -- The number of samples with this function as the leaf
  -- frame.
  self_count INT,
  -- The number of samples with this function appearing
  -- anywhere on the callstack.
  cumulative_count INT
) AS
SELECT r.*, a.cumulative_count
FROM _graph_aggregating_scan!(
  (
    SELECT id AS source_node_id, parent_id AS dest_node_id
    FROM _linux_perf_raw_callstacks
    WHERE parent_id IS NOT NULL
  ),
  (
    SELECT p.id, p.self_count AS cumulative_count
    FROM _linux_perf_raw_callstacks p
    LEFT JOIN _linux_perf_raw_callstacks c ON c.parent_id = p.id
    WHERE c.id IS NULL
  ),
  (cumulative_count),
  (
    WITH agg AS (
      SELECT t.id, SUM(t.cumulative_count) AS child_count
      FROM $table t
      GROUP BY t.id
    )
    SELECT
      a.id,
      a.child_count + r.self_count as cumulative_count
    FROM agg a
    JOIN _linux_perf_raw_callstacks r USING (id)
  )
) a
JOIN _linux_perf_raw_callstacks r USING (id)
ORDER BY r.id;
