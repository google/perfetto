--
-- Copyright 2022 The Android Open Source Project
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

INCLUDE PERFETTO MODULE android.freezer;

CREATE PERFETTO FUNCTION _extract_broadcast_process_name(name STRING)
RETURNS INT
AS
WITH
  pid_and_name AS (
    SELECT STR_SPLIT(STR_SPLIT($name, '/', 0), ' ', 1) AS value
  ),
  start AS (
    SELECT CAST(INSTR(value, ':') AS INT) + 1 AS value FROM pid_and_name
  )
SELECT SUBSTR(pid_and_name.value, start.value) FROM pid_and_name, start;

-- Provides a list of broadcast names and processes they were sent to by the
-- system_server process on U+ devices.
CREATE PERFETTO TABLE _android_broadcasts_minsdk_u(
  -- Intent action of the broadcast.
  intent_action STRING,
  -- Name of the process the broadcast was sent to.
  process_name STRING,
  -- Pid of the process the broadcast was sent to.
  pid STRING,
  -- Upid of the process the broadcast was sent to.
  upid STRING,
  -- Id of the broacast queue the broadcast was dispatched from.
  queue_id INT,
  -- Slice id of the broadcast dispatch.
  id INT,
  -- Timestamp the broadcast was dispatched.
  ts INT,
  -- Duration to dispatch the broadcast.
  dur INT,
  -- Track id the broadcast was dispatched from.
  track_id INT
) AS
WITH
  broadcast_queues AS (
    SELECT
      process_track.id,
      CAST(replace(str_split(process_track.name, '[', 1), ']', '') AS INT) AS queue_id
    FROM process_track
    JOIN process
      USING (upid)
    WHERE
      process_track.name GLOB 'BroadcastQueue.mRunning*'
      AND process.name = 'system_server'
  ),
  broadcast_process_running AS (
    SELECT
      slice.ts,
      slice.dur,
      _extract_broadcast_process_name(slice.name) AS process_name,
      CAST(str_split(str_split(str_split(slice.name, '/', 0), ' ', 1), ':', 0) AS INT) AS pid,
      queue_id
    FROM slice
    JOIN broadcast_queues
      ON broadcast_queues.id = slice.track_id
    WHERE slice.name GLOB '* running'
  )
SELECT
  str_split(str_split(slice.name, '/', 0), ' ', 1) AS intent_action,
  process_name,
  pid,
  _pid_to_upid(pid, slice.ts) AS upid,
  queue_id,
  slice.id,
  slice.ts,
  slice.dur,
  slice.track_id
FROM broadcast_process_running
JOIN broadcast_queues
  USING (queue_id)
JOIN slice
  ON (
    broadcast_process_running.ts < slice.ts
    AND slice.ts < broadcast_process_running.ts + broadcast_process_running.dur
    AND slice.track_id = broadcast_queues.id)
WHERE slice.name GLOB '* scheduled';
