--
-- Copyright 2023 The Android Open Source Project
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

-- List of all ANRs that occurred in the trace (one row per ANR).
--
-- @column process_name  Name of the process that triggered the ANR.
-- @column pid           PID of the process that triggered the ANR.
-- @column upid          UPID of the process that triggered the ANR.
-- @column error_id      UUID of the ANR (generated on the platform).
-- @column ts            Timestamp of the ANR.
-- @column subject       Subject line of the ANR.
CREATE VIEW android_anrs AS
-- Process and PID that ANRed.
WITH anr AS (
  SELECT
    -- Counter formats:
    -- v1: "ErrorId:<process_name>#<UUID>"
    -- v2: "ErrorId:<process_name> <pid>#<UUID>"
    STR_SPLIT(SUBSTR(STR_SPLIT(process_counter_track.name, '#', 0), 9), ' ', 0) AS process_name,
    CAST(STR_SPLIT(SUBSTR(STR_SPLIT(process_counter_track.name, '#', 0), 9), ' ', 1) AS INT32) AS pid,
    STR_SPLIT(process_counter_track.name, '#', 1) AS error_id,
    counter.ts
  FROM process_counter_track
  JOIN process USING (upid)
  JOIN counter ON (counter.track_id = process_counter_track.id)
  WHERE process_counter_track.name GLOB 'ErrorId:*'
    AND process.name = 'system_server'
),
-- ANR subject line.
subject AS (
  --- Counter format:
  --- "Subject(for ErrorId <UUID>):<subject>"
  SELECT
    SUBSTR(STR_SPLIT(process_counter_track.name, ')', 0), 21) AS error_id,
    SUBSTR(process_counter_track.name, length(STR_SPLIT(process_counter_track.name, ')', 0)) + 3) AS subject
  FROM process_counter_track
  JOIN process
  USING (upid)
  WHERE process_counter_track.name GLOB 'Subject(for ErrorId *'
  AND process.name = 'system_server'
)
SELECT
    anr.process_name,
    anr.pid,
    process.upid,
    anr.error_id,
    anr.ts,
    subject
FROM anr
LEFT JOIN subject USING (error_id)
LEFT JOIN process ON (process.pid = anr.pid);