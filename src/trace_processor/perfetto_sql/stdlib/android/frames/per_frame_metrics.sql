--
-- Copyright 2024 The Android Open Source Project
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

-- `actual_frame_timeline_slice` returns the same slice name for different layer IDs or tracks.
-- This happens because there can be multiple slices associated with a single frame_id.
-- We are interested in unique names(frame_id) and the respective counts.
-- This is a legacy table which is moved to this file because the android_frames_overrun table depends on it.
CREATE PERFETTO PIPELINE _frame_id_count_in_actual_timeline MATERIALIZED AS
FROM actual_frame_timeline_slice
|> AGGREGATE
     min(id) AS id,
     min(ts) AS ts,
     max(dur) AS dur,
     max(ts + dur) AS ts_end,
     count() AS count
   GROUP BY cast_int!(name) AS frame_id;

-- `expected_frame_timeline_slice` returns the same slice name for different tracks.
-- This happens because there can be multiple slices associated with a single frame_id.
-- We are interested in unique names(frame_id) and the respective counts.
-- This is a legacy table which is moved to this file because the android_frames_overrun table depends on it.
CREATE PERFETTO PIPELINE _frame_id_count_in_expected_timeline MATERIALIZED AS
FROM expected_frame_timeline_slice
|> AGGREGATE
     ANY_VALUE(id) AS id,
     count() AS count
   GROUP BY cast_int!(name) AS frame_id;

-- The amount by which each frame missed of hit its deadline. Negative if the
-- deadline was not missed. Frames are considered janky if `overrun` is
-- positive.
-- Calculated as the difference between the end of the
-- `expected_frame_timeline_slice` and `actual_frame_timeline_slice` for the
-- frame.
-- Availability: from S (API 31).
-- For Googlers: more details in go/android-performance-metrics-glossary.
CREATE PERFETTO PIPELINE android_frames_overrun(
  -- Frame id.
  frame_id LONG,
  -- Difference between `expected` and `actual` frame ends. Negative if frame
  -- didn't miss deadline.
  overrun LONG
) MATERIALIZED AS
FROM _frame_id_count_in_actual_timeline AS act
|> JOIN _frame_id_count_in_expected_timeline AS exp USING (frame_id)
|> JOIN slice AS act_slice ON (act.id = act_slice.id)
|> JOIN slice AS exp_slice ON (exp.id = exp_slice.id)
|> SELECT
     frame_id,
     (act_slice.ts + act_slice.dur) - (exp_slice.ts + exp_slice.dur) AS overrun;

-- How much time did the frame's Choreographer callbacks take.
CREATE PERFETTO PIPELINE android_frames_ui_time(
  -- Frame id
  frame_id LONG,
  -- UI time duration
  ui_time LONG
) MATERIALIZED AS
FROM android_frames_choreographer_do_frame AS f
|> JOIN slice USING (id)
|> SELECT frame_id, dur AS ui_time;

-- App Vsync delay for a frame. The time between the VSYNC-app signal and the
-- start of Choreographer work.
-- Calculated as time difference between the actual frame start (from
-- `actual_frame_timeline_slice`) and start of the `Choreographer#doFrame`
-- slice.
-- For Googlers: more details in go/android-performance-metrics-glossary.
CREATE PERFETTO PIPELINE android_app_vsync_delay_per_frame(
  -- Frame id
  frame_id LONG,
  -- App VSYNC delay.
  app_vsync_delay LONG
) MATERIALIZED AS
-- As there can be multiple `DrawFrame` slices, the `frames_surface_slices`
-- table contains multiple rows for the same `frame_id` which only differ on
-- `draw_frame_id`. As we don't care about `draw_frame_id` we can just collapse
-- them.
SUBPIPELINE distinct_frames AS (
  FROM android_frames
  |> AGGREGATE
       ANY_VALUE(do_frame_id) AS do_frame_id,
       ANY_VALUE(actual_frame_timeline_id) AS actual_frame_timeline_id,
       ANY_VALUE(expected_frame_timeline_id) AS expected_frame_timeline_id
     GROUP BY frame_id
)
FROM distinct_frames AS f
|> JOIN slice AS exp ON (f.expected_frame_timeline_id = exp.id)
|> JOIN slice AS act ON (f.actual_frame_timeline_id = act.id)
|> SELECT frame_id, act.ts - exp.ts AS app_vsync_delay;

-- How much time did the frame take across the UI Thread + RenderThread.
-- Calculated as sum of `app VSYNC delay` `Choreographer#doFrame` slice
-- duration and summed durations of all `DrawFrame` slices associated with this
-- frame.
-- Availability: from N (API 24).
-- For Googlers: more details in go/android-performance-metrics-glossary.
CREATE PERFETTO PIPELINE android_cpu_time_per_frame(
  -- Frame id
  frame_id LONG,
  -- Difference between actual timeline of the frame and
  -- `Choreographer#doFrame`. See `android_app_vsync_delay_per_frame` table for more details.
  app_vsync_delay LONG,
  -- Duration of `Choreographer#doFrame` slice.
  do_frame_dur DURATION,
  -- Duration of `DrawFrame` slice. Summed duration of all `DrawFrame`
  -- slices, if more than one. See `android_frames_draw_frame` for more details.
  draw_frame_dur DURATION,
  -- CPU time across the UI Thread + RenderThread.
  cpu_time LONG
) MATERIALIZED AS
SUBPIPELINE all_draw_frames AS (
  FROM android_frames_draw_frame
  |> JOIN slice USING (id)
  |> AGGREGATE sum(dur) AS draw_frame_dur GROUP BY frame_id
)
SUBPIPELINE distinct_frames AS (
  FROM android_frames
  |> AGGREGATE ANY_VALUE(do_frame_id) AS do_frame_id, ANY_VALUE(actual_frame_timeline_id) AS actual_frame_timeline_id GROUP BY frame_id
)
FROM android_app_vsync_delay_per_frame
|> JOIN all_draw_frames USING (frame_id)
|> JOIN distinct_frames AS f USING (frame_id)
|> JOIN slice AS do_frame ON (f.do_frame_id = do_frame.id)
|> SELECT
     frame_id,
     app_vsync_delay,
     do_frame.dur AS do_frame_dur,
     draw_frame_dur,
     app_vsync_delay + do_frame.dur + draw_frame_dur AS cpu_time;

-- CPU time of frames which don't have `android_cpu_time_per_frame` available.
-- Calculated as UI time of the frame + 5ms.
-- For Googlers: more details in go/android-performance-metrics-glossary.
CREATE PERFETTO PIPELINE _cpu_time_per_frame_fallback(
  -- Frame id.
  frame_id LONG,
  -- Estimated cpu time.
  estimated_cpu_time LONG
) MATERIALIZED AS
FROM android_frames_ui_time
|> SELECT frame_id, ui_time + time_from_ms(5) AS estimated_cpu_time;

CREATE PERFETTO PIPELINE _estimated_cpu_time_per_frame(
  frame_id LONG,
  cpu_time LONG
) MATERIALIZED AS
FROM _cpu_time_per_frame_fallback AS f
|> LEFT JOIN android_cpu_time_per_frame AS r USING (frame_id)
|> SELECT
     frame_id,
     iif(r.cpu_time IS NULL, f.estimated_cpu_time, r.cpu_time) AS cpu_time;

-- Aggregated stats of the frame.
--
-- For Googlers: more details in go/android-performance-metrics-glossary.
CREATE PERFETTO PIPELINE android_frame_stats(
  -- Frame id.
  frame_id LONG,
  -- The amount by which each frame missed of hit its deadline. See
  -- `android_frames_overrun` for details.
  overrun LONG,
  -- How much time did the frame take across the UI Thread + RenderThread.
  cpu_time LONG,
  -- How much time did the frame's Choreographer callbacks take.
  ui_time LONG,
  -- Was frame janky.
  was_jank BOOL,
  -- CPU time of the frame took over 20ms.
  was_slow_frame BOOL,
  -- CPU time of the frame took over 50ms.
  was_big_jank BOOL,
  -- CPU time of the frame took over 200ms.
  was_huge_jank BOOL
) MATERIALIZED AS
FROM android_frames_overrun
|> JOIN android_frames_ui_time USING (frame_id)
|> JOIN _estimated_cpu_time_per_frame USING (frame_id)
|> SELECT
     frame_id,
     overrun,
     cpu_time,
     ui_time,
     iif(overrun > 0, 1, NULL) AS was_jank,
     iif(cpu_time > time_from_ms(20), 1, NULL) AS was_slow_frame,
     iif(cpu_time > time_from_ms(50), 1, NULL) AS was_big_jank,
     iif(cpu_time > time_from_ms(200), 1, NULL) AS was_huge_jank;
