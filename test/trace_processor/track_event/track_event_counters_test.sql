--
-- Copyright 2020 The Android Open Source Project
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
  counter_track.name AS counter_name,
  process.name AS process,
  thread.name AS thread,
  thread_process.name AS thread_process,
  counter_track.unit AS unit,
  counter.ts,
  counter.value
FROM counter
LEFT JOIN counter_track ON counter.track_id = counter_track.id
LEFT JOIN process_counter_track ON counter.track_id = process_counter_track.id
LEFT JOIN process ON process_counter_track.upid = process.upid
LEFT JOIN thread_counter_track ON counter.track_id = thread_counter_track.id
LEFT JOIN thread ON thread_counter_track.utid = thread.utid
LEFT JOIN process thread_process ON thread.upid = thread_process.upid
ORDER BY ts ASC;
