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

CREATE PERFETTO FUNCTION _extract_anr_type(
    subject STRING
)
RETURNS STRING AS
SELECT
  CASE
    WHEN $subject IS NULL
    THEN NULL
    WHEN $subject GLOB 'Broadcast of*'
    THEN 'BROADCAST_OF_INTENT'
    WHEN $subject GLOB 'Input dispatching timed out*does not have a focused window*'
    THEN 'INPUT_DISPATCHING_TIMEOUT_NO_FOCUSED_WINDOW'
    WHEN $subject GLOB 'Input dispatching timed out*Waiting because no window has focus but there is a focused application*'
    THEN 'INPUT_DISPATCHING_TIMEOUT_NO_FOCUSED_WINDOW'
    WHEN $subject GLOB 'Input dispatching timed out*'
    THEN 'INPUT_DISPATCHING_TIMEOUT'
    WHEN $subject GLOB 'Context.startForegroundService() did not then call Service.startForeground()*'
    THEN 'START_FOREGROUND_SERVICE'
    WHEN $subject GLOB 'executing service*'
    THEN 'EXECUTING_SERVICE'
    WHEN $subject GLOB 'ContentProvider not responding*'
    THEN 'CONTENT_PROVIDER_NOT_RESPONDING'
    WHEN $subject GLOB 'App requested: Buffer processing hung up due to stuck fence. Indicates GPU hang'
    THEN 'GPU_HANG'
    WHEN $subject GLOB 'No response to onStartJob*'
    THEN 'JOB_SERVICE_START'
    WHEN $subject GLOB 'No response to onStopJob*'
    THEN 'JOB_SERVICE_STOP'
    WHEN $subject GLOB 'Timed out while trying to bind*'
    THEN 'JOB_SERVICE_BIND'
    WHEN $subject GLOB 'Process ProcessRecord{*} failed to complete startup'
    THEN 'BIND_APPLICATION'
    WHEN $subject GLOB 'A foreground service of FOREGROUND_SERVICE_TYPE_SHORT_SERVICE did not stop within a timeout*'
    THEN 'FOREGROUND_SHORT_SERVICE_TIMEOUT'
    WHEN $subject GLOB 'A foreground service of type*'
    THEN 'FOREGROUND_SERVICE_TIMEOUT'
    WHEN $subject GLOB 'App requested: *'
    THEN 'APP_TRIGGERED'
    WHEN $subject GLOB 'required notification not provided*'
    THEN 'JOB_SERVICE_NOTIFICATION_NOT_PROVIDED'
    ELSE 'UNKNOWN_ANR_TYPE'
  END;

-- List of all ANRs that occurred in the trace (one row per ANR).
CREATE PERFETTO VIEW android_anrs (
  -- Name of the process that triggered the ANR.
  process_name STRING,
  -- PID of the process that triggered the ANR.
  pid LONG,
  -- UPID of the process that triggered the ANR.
  upid JOINID(process.id),
  -- UUID of the ANR (generated on the platform).
  error_id STRING,
  -- Timestamp of the ANR.
  ts TIMESTAMP,
  -- Subject line of the ANR.
  subject STRING,
  -- Type of ANR
  anr_type STRING
) AS
-- Process and PID that ANRed.
WITH
  anr AS (
    SELECT
      -- Counter formats:
      -- v1: "ErrorId:<process_name>#<UUID>"
      -- v2: "ErrorId:<process_name> <pid>#<UUID>"
      str_split(substr(str_split(process_counter_track.name, '#', 0), 9), ' ', 0) AS process_name,
      cast_int!(STR_SPLIT(SUBSTR(STR_SPLIT(process_counter_track.name, '#', 0), 9), ' ', 1)) AS pid,
      str_split(process_counter_track.name, '#', 1) AS error_id,
      counter.ts
    FROM process_counter_track
    JOIN process
      USING (upid)
    JOIN counter
      ON (
        counter.track_id = process_counter_track.id
      )
    WHERE
      process_counter_track.name GLOB 'ErrorId:*' AND process.name = 'system_server'
  ),
  -- ANR subject line.
  subject AS (
    --- Counter format:
    --- "Subject(for ErrorId <UUID>):<subject>"
    SELECT
      substr(str_split(process_counter_track.name, ')', 0), 21) AS error_id,
      substr(
        process_counter_track.name,
        length(str_split(process_counter_track.name, ')', 0)) + 3
      ) AS subject
    FROM process_counter_track
    JOIN process
      USING (upid)
    WHERE
      process_counter_track.name GLOB 'Subject(for ErrorId *'
      AND process.name = 'system_server'
  )
SELECT
  anr.process_name,
  anr.pid,
  process.upid,
  anr.error_id,
  anr.ts,
  subject,
  _extract_anr_type(subject) AS anr_type
FROM anr
LEFT JOIN subject
  USING (error_id)
LEFT JOIN process
  ON (
    process.pid = anr.pid
  );
