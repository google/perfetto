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

-- The start of the launching event corresponds to the end of the AM handling
-- the startActivity intent, whereas the end corresponds to the first frame drawn.
-- Only successful app launches have a launching event.
DROP TABLE IF EXISTS launching_events;
CREATE TABLE launching_events AS
SELECT
  ts,
  dur,
  ts + dur AS ts_end,
  STR_SPLIT(s.name, ": ", 1) AS package_name
FROM slice s
JOIN process_track t ON s.track_id = t.id
JOIN process USING(upid)
WHERE s.name GLOB 'launching: *'
AND (process.name IS NULL OR process.name = 'system_server');

SELECT CREATE_FUNCTION(
  'ANDROID_SDK_LEVEL()',
  'INT', "
    SELECT int_value
    FROM metadata
    WHERE name = 'android_sdk_version'
  ");

-- Note: on Q, we didn't have Android fingerprints but we *did*
-- have ActivityMetricsLogger events so we will use this approach
-- if we see any such events.
SELECT CASE
  WHEN (
    ANDROID_SDK_LEVEL() >= 29
    OR (
      SELECT COUNT(1) FROM slice
      WHERE name GLOB 'MetricsLogger:*'
    ) > 0
  )
  THEN RUN_METRIC('android/startup/launches_minsdk29.sql')
  ELSE RUN_METRIC('android/startup/launches_maxsdk28.sql')
END;

-- Maps a launch to the corresponding set of processes that handled the
-- activity start. The vast majority of cases should be a single process.
-- However it is possible that the process dies during the activity launch
-- and is respawned.
DROP TABLE IF EXISTS launch_processes;
CREATE TABLE launch_processes(launch_id INT, upid BIG INT);

INSERT INTO launch_processes
SELECT launches.id, process.upid
FROM launches
LEFT JOIN package_list ON (
  launches.package = package_list.package_name
)
JOIN process ON (
  launches.package = process.name OR
  process.uid = package_list.uid
)
JOIN thread ON (
  process.upid = thread.upid AND
  process.pid = thread.tid
)
WHERE (process.start_ts IS NULL OR process.start_ts < launches.ts_end)
AND (thread.end_ts IS NULL OR thread.end_ts > launches.ts_end)
ORDER BY process.start_ts DESC;
