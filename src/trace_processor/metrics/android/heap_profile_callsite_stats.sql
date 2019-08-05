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

CREATE VIEW totals_per_callsite AS
SELECT upid, callsite_id, SUM(count) AS total_count, SUM(size) AS total_size
FROM heap_profile_allocation
WHERE count > 0
GROUP BY 1, 2;

CREATE VIEW deltas_per_callsite AS
SELECT upid, callsite_id, SUM(count) AS delta_count, SUM(size) AS delta_size
FROM heap_profile_allocation
GROUP BY 1, 2;

-- For each callsite ID traverse all frames to the root.
CREATE TABLE flattened_callsite AS
WITH RECURSIVE callsite_parser(callsite_id, current_id, position) AS (
  SELECT id, id, 0 FROM heap_profile_callsite
  UNION
  SELECT callsite_id, parent_id, position + 1
  FROM callsite_parser JOIN heap_profile_callsite ON heap_profile_callsite.id = current_id
  WHERE heap_profile_callsite.depth > 0
)
SELECT *
FROM callsite_parser;

-- Join with the frames table to get the symbol names.
-- Output order for position matters (as will be the order in the subsequent aggregate operations).
-- We use the cross join to force the join order between virtual and non-virtual tables.
CREATE VIEW frames_by_callsite_id AS
SELECT
  callsite_id,
  position,
  HeapProfileCallsiteStats_Frame('name', heap_profile_frame.name) AS frame_proto
FROM flattened_callsite
CROSS JOIN heap_profile_callsite
CROSS JOIN heap_profile_frame
WHERE
  flattened_callsite.current_id = heap_profile_callsite.id
  AND heap_profile_callsite.frame_id = heap_profile_frame.id
ORDER BY callsite_id, position;

-- Map callsite ID to proto.
CREATE TABLE callsites_by_id AS
SELECT
  callsite_id,
  HeapProfileCallsiteStats_Callsite('frame', RepeatedField(frame_proto))
    AS callsite_proto
FROM frames_by_callsite_id
GROUP BY callsite_id;

CREATE VIEW callsite_stats AS
SELECT
  upid,
  callsite_proto,
  totals.total_count,
  totals.total_size,
  deltas.delta_count,
  deltas.delta_size
FROM totals_per_callsite totals
JOIN deltas_per_callsite deltas USING (upid, callsite_id)
JOIN callsites_by_id USING (callsite_id);

CREATE VIEW callsite_stats_proto AS
SELECT
  upid,
  RepeatedField(
    HeapProfileCallsiteStats_CallsiteStats(
      'callsite', callsite_proto,
      'total_count', total_count,
      'total_bytes', total_size,
      'delta_count', delta_count,
      'delta_bytes', delta_size
    )
  ) AS repeated_callsite_stats
FROM callsite_stats
GROUP BY 1;

CREATE VIEW instance_stats_view AS
SELECT HeapProfileCallsiteStats_InstanceStats(
    'pid', process.pid,
    'process_name', process.name,
    'callsite_stats', repeated_callsite_stats
) AS instance_stats_proto
FROM callsite_stats_proto JOIN process USING (upid);

CREATE VIEW heap_profile_callsite_stats_output AS
SELECT HeapProfileCallsiteStats(
  'instance_stats',
  (SELECT RepeatedField(instance_stats_proto) FROM instance_stats_view)
);
