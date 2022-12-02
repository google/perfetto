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

-- Provides a list of broadcast names and processes they were sent to by the
-- system_server process on U+ devices.
--
-- @column type          The name of the broadcast type which was sent.
-- @column process_name  The process name the broadcast was sent to.
-- @column queue_name    The name of the broacast queue the broadcast was
--                       dispatched from.
CREATE VIEW experimental_android_broadcasts_minsdk_u AS
WITH
broadcast_queues AS (
  SELECT process_track.id, process_track.name AS queue_name
  FROM process_track
  JOIN process USING (upid)
  WHERE
    process_track.name GLOB 'BroadcastQueue.mRunning*'
    AND process.name = 'system_server'
),
broadcast_process_running AS (
  SELECT ts, dur, str_split(slice.name, '/', 0) AS process_name, queue_name
  FROM slice
  JOIN broadcast_queues ON broadcast_queues.id = slice.track_id
  WHERE slice.name GLOB '* running'
)
SELECT str_split(slice.name, '/', 0) AS type, process_name, queue_name
FROM broadcast_process_running
JOIN broadcast_queues USING (queue_name)
JOIN slice ON (
  broadcast_process_running.ts < slice.ts
  AND slice.ts < broadcast_process_running.ts + broadcast_process_running.dur
  AND slice.track_id = broadcast_queues.id
  )
WHERE slice.name GLOB '* scheduled';
