--
-- Copyright 2024 The Android Open Source Project
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

-- All scheduled jobs and their latencies.
CREATE PERFETTO TABLE android_job_scheduler_events (
  -- Id of the scheduled job assigned by the app developer.
  job_id INT,
  -- Uid of the process running the scheduled job.
  uid INT,
  -- Package name of the process running the scheduled job.
  package_name STRING,
  -- Service component name of the scheduled job.
  job_service_name STRING,
  -- Thread track id of the job scheduler event slice.
  track_id INT,
  -- Slice id of the job scheduler event slice.
  id INT,
  -- Timestamp the job was scheduled.
  ts INT,
  -- Duration of the scheduled job.
  dur INT
  ) AS
SELECT
  CAST(STR_SPLIT(slice.name, '#', 1) AS INT) AS job_id,
  CAST(STR_SPLIT(STR_SPLIT(slice.name, '<', 1), '>', 0) AS INT) AS uid,
  STR_SPLIT(STR_SPLIT(slice.name, '>', 1), '/', 0) AS package_name,
  STR_SPLIT(STR_SPLIT(slice.name, '/', 1), '#', 0) AS job_service_name,
  track_id,
  slice.id,
  slice.ts,
  IIF(slice.dur = -1, trace_end() - slice.ts, slice.dur) AS dur
FROM
  slice
JOIN process_track
  ON slice.track_id = process_track.id
JOIN process
  ON process.upid = process_track.upid
WHERE
  process.name = 'system_server'
  AND slice.name GLOB '*job*'
  AND process_track.name = 'JobScheduler';
