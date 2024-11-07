
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

CREATE PERFETTO TABLE _android_heap_profile_raw_callstacks AS
WITH metrics AS MATERIALIZED (
  SELECT
    callsite_id,
    SUM(size) AS self_size,
    SUM(MAX(size, 0)) AS self_alloc_size
  FROM heap_profile_allocation
  GROUP BY callsite_id
)
SELECT
  c.id,
  c.parent_id,
  c.name,
  c.mapping_name,
  c.source_file,
  c.line_number,
  IFNULL(m.self_size, 0) AS self_size,
  IFNULL(m.self_alloc_size, 0) AS self_alloc_size
FROM _callstacks_for_stack_profile_samples!(metrics) c
LEFT JOIN metrics m USING (callsite_id);

CREATE PERFETTO TABLE _android_heap_profile_cumulatives AS
SELECT a.*
FROM _graph_aggregating_scan!(
  (
    SELECT id AS source_node_id, parent_id AS dest_node_id
    FROM _android_heap_profile_raw_callstacks
    WHERE parent_id IS NOT NULL
  ),
  (
    SELECT
      p.id,
      p.self_size AS cumulative_size,
      p.self_alloc_size AS cumulative_alloc_size
    FROM _android_heap_profile_raw_callstacks p
    LEFT JOIN _android_heap_profile_raw_callstacks c ON c.parent_id = p.id
    WHERE c.id IS NULL
  ),
  (cumulative_size, cumulative_alloc_size),
  (
    WITH agg AS (
      SELECT
        t.id,
        SUM(t.cumulative_size) AS child_size,
        SUM(t.cumulative_alloc_size) AS child_alloc_size
      FROM $table t
      GROUP BY t.id
    )
    SELECT
      a.id,
      a.child_size + r.self_size as cumulative_size,
      a.child_alloc_size + r.self_alloc_size AS cumulative_alloc_size
    FROM agg a
    JOIN _android_heap_profile_raw_callstacks r USING (id)
  )
) a;

-- Table summarising the amount of memory allocated by each
-- callstack as seen by Android native heap profiling (i.e.
-- profiling information collected by heapprofd).
--
-- Note: this table collapses data from all processes together
-- into a single table.
CREATE PERFETTO TABLE android_heap_profile_summary_tree(
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
  -- The amount of memory allocated and *not freed* with this
  -- function as the leaf frame.
  self_size INT,
  -- The amount of memory allocated and *not freed* with this
  -- function appearing anywhere on the callstack.
  cumulative_size INT,
  -- The amount of memory allocated with this function as the leaf
  -- frame. This may include memory which was later freed.
  self_alloc_size INT,
  -- The amount of memory allocated with this function appearing
  -- anywhere on the callstack. This may include memory which was
  -- later freed.
  cumulative_alloc_size INT
) AS
SELECT
  id,
  parent_id,
  name,
  mapping_name,
  source_file,
  line_number,
  self_size,
  cumulative_size,
  self_alloc_size,
  cumulative_alloc_size
FROM _android_heap_profile_raw_callstacks r
JOIN _android_heap_profile_cumulatives a USING (id);
