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

-- Class-level breakdown of the Java heap using android_heap_graph_class_aggregation.
-- Uses tree_structural_partition_by_group under the hood so dominated_size_bytes
-- and dominated_native_size_bytes are strictly non-overlapping and never
-- double-counted when instances of a class dominate other instances of the same class.
INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_class_aggregation;

SELECT
  COALESCE(pr.name, 'pid=' || pr.pid) AS process_name,
  a.upid,
  a.graph_sample_ts,
  a.type_name AS class_name,
  a.is_libcore_or_array,
  SUM(a.obj_count) AS obj_count,
  SUM(a.size_bytes) AS size_bytes,
  SUM(a.native_size_bytes) AS native_size_bytes,
  SUM(a.reachable_obj_count) AS reachable_obj_count,
  SUM(a.reachable_size_bytes) AS reachable_size_bytes,
  SUM(a.reachable_native_size_bytes) AS reachable_native_size_bytes,
  SUM(a.dominated_obj_count) AS dominated_obj_count,
  SUM(a.dominated_size_bytes) AS dominated_size_bytes,
  SUM(a.dominated_native_size_bytes) AS dominated_native_size_bytes
FROM android_heap_graph_class_aggregation AS a
JOIN process AS pr ON a.upid = pr.id
GROUP BY
  process_name,
  a.upid,
  a.graph_sample_ts,
  a.type_name,
  a.is_libcore_or_array
ORDER BY
  (SUM(a.dominated_size_bytes) + SUM(a.dominated_native_size_bytes)) DESC,
  SUM(a.reachable_obj_count) DESC;
