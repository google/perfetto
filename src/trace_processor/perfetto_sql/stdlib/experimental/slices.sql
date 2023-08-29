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

INCLUDE PERFETTO MODULE common.slices;

-- All slices with related process and thread info if available. Unlike
-- `thread_slice` and `process_slice`, this view contains all slices,
-- with thread- and process-related columns set to NULL if the slice
-- is not associated with a thread or a process.
--
-- @column id                 Alias for `slice.id`.
-- @column type               Alias for `slice.type`.
-- @column ts                 Alias for `slice.ts`.
-- @column dur                Alias for `slice.dur`.
-- @column category           Alias for `slice.category`.
-- @column name               Alias for `slice.name`.
-- @column track_id           Alias for `slice.track_id`.
-- @column track_name         Alias for `track.name`.
-- @column thread_name        Alias for `thread.name`.
-- @column utid               Alias for `thread.utid`.
-- @column tid                Alias for `thread.tid`
-- @column process_name       Alias for `process.name`.
-- @column upid               Alias for `process.upid`.
-- @column pid                Alias for `process.pid`.
-- @column depth              Alias for `slice.depth`.
-- @column parent_id          Alias for `slice.parent_id`.
-- @column arg_set_id         Alias for `slice.arg_set_id`.
-- @column thread_ts          Alias for `slice.thread_ts`.
-- @column thread_dur         Alias for `slice.thread_dur`.
CREATE VIEW experimental_slice_with_thread_and_process_info AS
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
