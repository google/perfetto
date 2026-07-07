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

INCLUDE PERFETTO MODULE android.memory.heap_graph.class_summary_tree;

SELECT
  count(self_count) AS total_unique_paths_to_gc_root,
  name AS class_name,
  SUM(self_count) AS total_objects,
  sum(cumulative_size) AS total_objects_memory_consumption,
  MIN(self_size) AS single_object_self_size,
  MIN(cumulative_size) AS single_object_cumulative_size
FROM android_heap_graph_class_summary_tree
GROUP BY
  name
ORDER BY
  count(*) DESC;
