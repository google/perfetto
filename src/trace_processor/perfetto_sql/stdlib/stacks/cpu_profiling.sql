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
INCLUDE PERFETTO MODULE linux.perf.samples;

CREATE PERFETTO TABLE _cpu_profiling_raw_callstacks AS
SELECT *
FROM _callstacks_for_cpu_profile_stack_samples!(
  (SELECT s.callsite_id FROM cpu_profile_stack_sample s)
) c
ORDER BY c.id;

-- Table summarising the callstacks captured during any CPU
-- profiling which occurred during the trace.
--
-- Specifically, this table returns a tree containing all
-- the callstacks seen during the trace with `self_count`
-- equal to the number of samples with that frame as the
-- leaf and `cumulative_count` equal to the number of
-- samples with the frame anywhere in the tree.
--
-- Currently, this table is backed by the following data
-- sources:
--  * any perf sampling
--  * generic CPU profiling (e.g. Chrome, ad-hoc traces)
CREATE PERFETTO TABLE cpu_profiling_summary_tree(
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
SELECT
  id,
  parent_id,
  name,
  mapping_name,
  source_file,
  line_number,
  SUM(self_count) AS self_count,
  SUM(cumulative_count) AS cumulative_count
FROM (
  -- Generic CPU profiling.
  SELECT r.*, a.cumulative_count
  FROM _callstacks_self_to_cumulative!((
    SELECT id, parent_id, self_count
    FROM _cpu_profiling_raw_callstacks
  )) a
  JOIN _cpu_profiling_raw_callstacks r USING (id)
  UNION ALL
  -- Linux perf sampling.
  SELECT *
  FROM linux_perf_samples_summary_tree
)
GROUP BY id
ORDER BY id;
