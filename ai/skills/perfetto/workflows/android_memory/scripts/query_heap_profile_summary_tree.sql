-- Copyright (C) 2026 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--      http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- Extracts per-function/frame cumulative and self allocation metrics from
-- android_heap_profile_summary_tree for Java and Native heap profiles.
INCLUDE PERFETTO MODULE android.memory.heap_profile.summary_tree;

CREATE OR REPLACE PERFETTO TABLE _profile_dominant_process AS
SELECT a.upid, COALESCE(p.name, "pid=" || p.pid) AS process_name, a.heap_name
FROM heap_profile_allocation AS a
JOIN process AS p USING (upid)
GROUP BY
  a.upid,
  process_name,
  a.heap_name
ORDER BY
  SUM(MAX(a.size, 0)) DESC
LIMIT 1;

SELECT
  (SELECT process_name FROM _profile_dominant_process) AS process_name,
  (SELECT upid FROM _profile_dominant_process) AS upid,
  (SELECT heap_name FROM _profile_dominant_process) AS heap_name,
  name AS function_name,
  COALESCE(mapping_name, "") AS mapping_name,
  SUM(self_size) AS self_size,
  MAX(cumulative_size) AS cumulative_size,
  SUM(self_alloc_size) AS self_alloc_size,
  MAX(cumulative_alloc_size) AS cumulative_alloc_size
FROM android_heap_profile_summary_tree
WHERE
  self_size > 0
  OR self_alloc_size > 0
  OR cumulative_size > 0
  OR cumulative_alloc_size > 0
GROUP BY
  function_name,
  mapping_name
ORDER BY
  cumulative_alloc_size DESC,
  cumulative_size DESC;
