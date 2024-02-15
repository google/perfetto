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

-- All slices with related process and thread info if available. Unlike
-- `thread_slice` and `process_slice`, this view contains all slices,
-- with thread- and process-related columns set to NULL if the slice
-- is not associated with a thread or a process.
CREATE PERFETTO VIEW _slice_with_thread_and_process_info(
  -- Alias for `slice.id`.
  id INT,
  -- Alias for `slice.type`.
  type STRING,
  -- Alias for `slice.ts`.
  ts INT,
  -- Alias for `slice.dur`.
  dur INT,
  -- Alias for `slice.category`.
  category STRING,
  -- Alias for `slice.name`.
  name STRING,
  -- Alias for `slice.track_id`.
  track_id INT,
  -- Alias for `track.name`.
  track_name STRING,
  -- Alias for `thread.name`.
  thread_name STRING,
  -- Alias for `thread.utid`.
  utid INT,
  -- Alias for `thread.tid`
  tid INT,
  -- Alias for `process.name`.
  process_name STRING,
  -- Alias for `process.upid`.
  upid INT,
  -- Alias for `process.pid`.
  pid INT,
  -- Alias for `slice.depth`.
  depth INT,
  -- Alias for `slice.parent_id`.
  parent_id INT,
  -- Alias for `slice.arg_set_id`.
  arg_set_id INT,
  -- Alias for `slice.thread_ts`.
  thread_ts INT,
  -- Alias for `slice.thread_dur`.
  thread_dur INT
) AS
SELECT
  slice.id,
  slice.type,
  slice.ts,
  slice.dur,
  slice.category,
  slice.name,
  slice.track_id,
  track.name AS track_name,
  thread.name AS thread_name,
  thread.utid,
  thread.tid,
  COALESCE(process1.name, process2.name) AS process_name,
  COALESCE(process1.upid, process2.upid) AS upid,
  COALESCE(process1.pid, process2.pid) AS pid,
  slice.depth,
  slice.parent_id,
  slice.arg_set_id,
  slice.thread_ts,
  slice.thread_dur
FROM slice
JOIN track ON slice.track_id = track.id
LEFT JOIN thread_track ON slice.track_id = thread_track.id
LEFT JOIN thread USING (utid)
LEFT JOIN process process1 ON thread.upid = process1.upid
LEFT JOIN process_track ON slice.track_id = process_track.id
LEFT JOIN process process2 ON process_track.upid = process2.upid;
