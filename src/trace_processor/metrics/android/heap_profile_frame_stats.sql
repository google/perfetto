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

CREATE VIEW totals_per_frame AS
SELECT upid, frame_id, SUM(count) AS total_count, SUM(size) AS total_size
FROM heap_profile_allocation hpa
JOIN heap_profile_callsite hpc
ON hpa.callsite_id = hpc.id
WHERE count > 0
GROUP BY 1, 2;

CREATE VIEW deltas_per_frame AS
SELECT upid, frame_id, SUM(count) AS delta_count, SUM(size) AS delta_size
FROM heap_profile_allocation hpa
JOIN heap_profile_callsite hpc
ON hpa.callsite_id = hpc.id
GROUP BY 1, 2;

CREATE VIEW frame_stats AS
SELECT
  upid,
  HeapProfileFrameStats_Frame('name', frame.name) AS frame_proto,
  totals.total_count,
  totals.total_size,
  deltas.delta_count,
  deltas.delta_size
FROM totals_per_frame totals
JOIN deltas_per_frame deltas USING (upid, frame_id)
JOIN heap_profile_frame frame ON totals.frame_id = frame.id;

CREATE VIEW frame_stats_proto AS
SELECT
  upid,
  RepeatedField(
    HeapProfileFrameStats_FrameStats(
      'frame', frame_proto,
      'total_count', total_count,
      'total_bytes', total_size,
      'delta_count', delta_count,
      'delta_bytes', delta_size
    )
  ) AS repeated_frame_stats
FROM frame_stats
GROUP BY 1;

CREATE VIEW instance_stats_view AS
SELECT HeapProfileFrameStats_ProcessInstanceStats(
    'process_name', process.name,
    'frame_stats', repeated_frame_stats
) AS instance_stats_proto
FROM frame_stats_proto JOIN process USING (upid);

CREATE VIEW heap_profile_frame_stats_output AS
SELECT HeapProfileFrameStats(
  'instance_stats', (SELECT RepeatedField(instance_stats_proto) FROM instance_stats_view)
);
