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

-- Count Binder transactions per process.
--
-- @column process_name  Name of the process that started the binder transaction.
-- @column pid           PID of the process that started the binder transaction.
-- @column slice_name    Name of the slice with binder transaction.
-- @column event_count   Number of binder transactions in process in slice.
CREATE VIEW android_binder_metrics_by_process AS
SELECT
  process.name AS process_name,
  process.pid AS pid,
  slice.name AS slice_name,
  COUNT(*) AS event_count
FROM slice
INNER JOIN thread_track ON slice.track_id = thread_track.id
INNER JOIN thread ON thread.utid = thread_track.utid
INNER JOIN process ON thread.upid = process.upid
WHERE
  slice.name GLOB 'binder*'
GROUP BY
  process_name,
  slice_name;
