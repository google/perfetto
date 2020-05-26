--
-- Copyright 2019 The Android Open Source Project
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
--

SELECT RUN_METRIC('android/process_metadata.sql');

CREATE VIEW total_size_samples AS
SELECT upid, graph_sample_ts, SUM(self_size) AS total_size
FROM heap_graph_object
GROUP BY 1, 2;

CREATE VIEW total_reachable_size_samples AS
SELECT upid, graph_sample_ts, SUM(self_size) AS total_reachable_size
FROM heap_graph_object
WHERE reachable = TRUE
GROUP BY 1, 2;

CREATE TABLE heap_graph_samples AS
SELECT upid, graph_sample_ts, total_size, total_reachable_size
FROM total_size_samples JOIN total_reachable_size_samples
USING (upid, graph_sample_ts);

CREATE VIEW heap_graph_sample_protos AS
SELECT
  upid,
  JavaHeapStats_Sample(
    'ts', graph_sample_ts,
    'heap_size', total_size,
    'reachable_heap_size', total_reachable_size
  ) sample_proto
FROM heap_graph_samples;

CREATE TABLE heap_graph_instance_stats AS
SELECT
  upid,
  process_metadata.metadata AS process_metadata,
  RepeatedField(sample_proto) AS sample_protos
FROM heap_graph_sample_protos JOIN process_metadata USING (upid)
GROUP BY 1, 2;

CREATE VIEW java_heap_stats_output AS
SELECT JavaHeapStats(
  'instance_stats', RepeatedField(JavaHeapStats_InstanceStats(
    'upid', upid,
    'process', process_metadata,
    'samples', sample_protos
  )))
FROM heap_graph_instance_stats;
