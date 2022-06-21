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

-- Helper function to build a Slice proto from a duration.
SELECT CREATE_FUNCTION('STARTUP_SLICE_PROTO(dur INT)', 'PROTO', '
  SELECT AndroidStartupMetric_Slice(
    "dur_ns", $dur,
    "dur_ms", $dur / 1e6
  )
');

-- View containing all the slices for all launches. Generally, this view
-- should not be used. Instead, one of the helper functions below which wrap
-- this view should be used.
DROP VIEW IF EXISTS thread_slices_for_all_launches;
CREATE VIEW thread_slices_for_all_launches AS
SELECT
  launch_threads.ts AS launch_ts,
  launch_threads.launch_id AS launch_id,
  launch_threads.utid AS utid,
  launch_threads.thread_name AS thread_name,
  launch_threads.is_main_thread AS is_main_thread,
  slice.arg_set_id AS arg_set_id,
  slice.name AS slice_name,
  slice.ts AS slice_ts,
  slice.dur AS slice_dur
FROM launch_threads
JOIN thread_track USING (utid)
JOIN slice ON (slice.track_id = thread_track.id)
WHERE slice.ts BETWEEN launch_threads.ts AND launch_threads.ts + launch_threads.dur;

-- Given a launch id and GLOB for a slice name, returns columns for matching slices.
SELECT CREATE_VIEW_FUNCTION(
  'SLICES_FOR_LAUNCH_AND_SLICE_NAME(launch_id INT, slice_name STRING)',
  'slice_name STRING, slice_ts INT, slice_dur INT, thread_name STRING, arg_set_id INT', '
    SELECT slice_name, slice_ts, slice_dur, thread_name, arg_set_id
    FROM thread_slices_for_all_launches
    WHERE launch_id = $launch_id AND slice_name GLOB $slice_name
  '
);

-- Given a launch id and GLOB for a slice name, returns the startup slice proto,
-- summing the slice durations across the whole startup.
SELECT CREATE_FUNCTION(
  'DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launch_id LONG, slice_name STRING)',
  'PROTO', '
    SELECT NULL_IF_EMPTY(STARTUP_SLICE_PROTO(SUM(slice_dur)))
    FROM thread_slices_for_all_launches
    WHERE launch_id = $launch_id AND slice_name GLOB $slice_name
  '
);

-- Given a launch id and GLOB for a slice name, returns the startup slice proto by
-- taking the duration between the start of the launch and start of the slice.
SELECT CREATE_FUNCTION(
  'LAUNCH_TO_MAIN_THREAD_SLICE_PROTO(launch_id INT, slice_name STRING)',
  'PROTO', '
    SELECT STARTUP_SLICE_PROTO(slice_ts - launch_ts)
    FROM thread_slices_for_all_launches
    WHERE slice_name GLOB $slice_name AND launch_id = $launch_id AND is_main_thread
  '
);
