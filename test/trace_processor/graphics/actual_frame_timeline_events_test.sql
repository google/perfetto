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

SELECT ts, dur, process.pid AS pid, display_frame_token, surface_frame_token, layer_name,
  present_type, on_time_finish, gpu_composition, jank_type, prediction_type, jank_tag
FROM
  (SELECT t.*, process_track.name AS track_name FROM
    process_track LEFT JOIN actual_frame_timeline_slice t
    ON process_track.id = t.track_id) s
JOIN process USING(upid)
WHERE s.track_name = 'Actual Timeline'
ORDER BY ts;
