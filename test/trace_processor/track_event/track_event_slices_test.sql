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
SELECT
  track.name AS track,
  process.name AS process,
  thread.name AS thread,
  thread_process.name AS thread_process,
  slice.ts,
  slice.dur,
  slice.category,
  slice.name
FROM slice
LEFT JOIN track ON slice.track_id = track.id
LEFT JOIN process_track ON slice.track_id = process_track.id
LEFT JOIN process ON process_track.upid = process.upid
LEFT JOIN thread_track ON slice.track_id = thread_track.id
LEFT JOIN thread ON thread_track.utid = thread.utid
LEFT JOIN process thread_process ON thread.upid = thread_process.upid
ORDER BY ts ASC;
