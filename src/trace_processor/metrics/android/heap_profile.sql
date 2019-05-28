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

-- Resolve all callsites with a positive allocation
CREATE TABLE positive_callsite AS
SELECT
  upid,
  callsite_id,
  SUM(count) AS allocs_self_count,
  SUM(size) AS allocs_self_bytes
FROM heap_profile_allocation
GROUP BY callsite_id
HAVING allocs_self_count > 0;

-- For each callsite ID traverse all frames to the root.
CREATE TABLE flattened_callsite AS
WITH RECURSIVE callsite_parser(upid, binding_id, current_id, position) AS (
    SELECT upid, callsite_id, callsite_id, 0 FROM positive_callsite
    UNION
    SELECT upid, binding_id, parent_id, position + 1
    FROM callsite_parser JOIN heap_profile_callsite ON heap_profile_callsite.id = current_id
    WHERE heap_profile_callsite.depth > 0
)
SELECT *
FROM callsite_parser;

-- Join with the frames table to get the symbol names.
-- Output order for position matters (as will be the order in the subsequent aggregate operations).
-- We use the cross join to force the join order between virtual and non-virtual tables.
CREATE VIEW frames_by_binding_id AS
SELECT
  binding_id,
  position,
  HeapProfile_Frame('name', heap_profile_frame.name) AS frame_proto
FROM flattened_callsite
CROSS JOIN heap_profile_callsite
CROSS JOIN heap_profile_frame
WHERE TRUE
AND flattened_callsite.current_id = heap_profile_callsite.id
AND heap_profile_callsite.frame_id = heap_profile_frame.id
ORDER BY binding_id, position;

CREATE TABLE callsites_by_binding_id AS
SELECT binding_id, HeapProfile_Callsite('frame', RepeatedField(frame_proto)) AS callsite_proto
FROM frames_by_binding_id
GROUP BY binding_id;

CREATE VIEW callsite_stats AS
SELECT
  upid,
  callsites_by_binding_id.callsite_proto AS callsite_proto,
  allocs_self_count,
  allocs_self_bytes
FROM positive_callsite JOIN callsites_by_binding_id
ON positive_callsite.callsite_id = callsites_by_binding_id.binding_id;

-- For each process instance, emit all relevant callsites.
CREATE VIEW process_profile_view AS
SELECT
  HeapProfile_PerProcess(
    'process_name', process.name,
    'callsite_stats', (
      SELECT RepeatedField(
        HeapProfile_CallsiteStats(
          'callsite', callsite_proto,
          'allocs_self_count', allocs_self_count,
          'allocs_self_bytes', allocs_self_bytes
        )
      )
      FROM callsite_stats
      WHERE callsite_stats.upid = hprof.upid
    )
  ) AS process_profile_proto
FROM (SELECT DISTINCT upid FROM flattened_callsite) AS hprof
JOIN process ON hprof.upid = process.upid;

CREATE VIEW heap_profile_output AS
SELECT HeapProfile(
  'profile', (SELECT RepeatedField(process_profile_proto) FROM process_profile_view)
);
