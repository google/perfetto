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

INCLUDE PERFETTO MODULE time.conversion;
INCLUDE PERFETTO MODULE android.frames.timeline;

-- The amount by which each frame missed of hit its deadline. Negative if the
-- deadline was not missed. Frames are considered janky if `overrun` is
-- positive.
-- Calculated as the difference between the end of the
-- `expected_frame_timeline_slice` and `actual_frame_timeline_slice` for the
-- frame.
-- Availability: from S (API 31).
-- For Googlers: more details in go/android-performance-metrics-glossary.
CREATE PERFETTO TABLE android_frames_overrun(
    -- Frame id.
    frame_id INT,
    -- Difference between `expected` and `actual` frame ends. Negative if frame
    -- didn't miss deadline.
    overrun INT
) AS
SELECT
    frame_id,
    (act_slice.ts + act_slice.dur) - (exp_slice.ts + exp_slice.dur) AS overrun
FROM _distinct_from_actual_timeline_slice act
JOIN _distinct_from_expected_timeline_slice exp USING (frame_id)
JOIN slice act_slice ON (act.id = act_slice.id)
JOIN slice exp_slice ON (exp.id = exp_slice.id);

-- How much time did the frame's Choreographer callbacks take.
CREATE PERFETTO TABLE android_frames_ui_time(
    -- Frame id
    frame_id INT,
    -- UI time duration
    ui_time INT
) AS
SELECT
    frame_id,
    dur AS ui_time
FROM android_frames_choreographer_do_frame f
JOIN slice USING (id);

-- App Vsync delay for a frame. The time between the VSYNC-app signal and the
-- start of Choreographer work.
-- Calculated as time difference between the actual frame start (from
-- `actual_frame_timeline_slice`) and start of the `Choreographer#doFrame`
-- slice.
-- For Googlers: more details in go/android-performance-metrics-glossary.
CREATE PERFETTO TABLE android_app_vsync_delay_per_frame(
    -- Frame id
    frame_id INT,
    -- App VSYNC delay.
    app_vsync_delay INT
) AS
-- As there can be multiple `DrawFrame` slices, the `frames_surface_slices`
-- table contains multiple rows for the same `frame_id` which only differ on
-- `draw_frame_id`. As we don't care about `draw_frame_id` we can just collapse
-- them.
WITH distinct_frames AS (
    SELECT
        frame_id,
        do_frame_id,
        actual_frame_timeline_id,
        expected_frame_timeline_id
    FROM android_frames
    GROUP BY 1
)
SELECT
    frame_id,
    act.ts - exp.ts AS app_vsync_delay
FROM distinct_frames f
JOIN slice exp ON (f.expected_frame_timeline_id = exp.id)
JOIN slice act ON (f.actual_frame_timeline_id = act.id);

-- How much time did the frame take across the UI Thread + RenderThread.
-- Calculated as sum of `app VSYNC delay` `Choreographer#doFrame` slice
-- duration and summed durations of all `DrawFrame` slices associated with this
-- frame.
-- Availability: from N (API 24).
-- For Googlers: more details in go/android-performance-metrics-glossary.
CREATE PERFETTO TABLE android_cpu_time_per_frame(
    -- Frame id
    frame_id INT,
    -- Difference between actual timeline of the frame and
    -- `Choreographer#doFrame`. See `android_app_vsync_delay_per_frame` table for more details.
    app_vsync_delay INT,
    -- Duration of `Choreographer#doFrame` slice.
    do_frame_dur INT,
    -- Duration of `DrawFrame` slice. Summed duration of all `DrawFrame`
    -- slices, if more than one. See `android_frames_draw_frame` for more details.
    draw_frame_dur INT,
    -- CPU time across the UI Thread + RenderThread.
    cpu_time INT
) AS
WITH all_draw_frames AS (
SELECT
    frame_id,
    SUM(dur) as draw_frame_dur
FROM android_frames_draw_frame
JOIN slice USING (id)
GROUP BY frame_id
),
distinct_frames AS (
    SELECT
        frame_id,
        do_frame_id,
        actual_frame_timeline_id
    FROM android_frames
    GROUP BY 1
)
SELECT
    frame_id,
    app_vsync_delay,
    do_frame.dur AS do_frame_dur,
    draw_frame_dur,
    app_vsync_delay + do_frame.dur + draw_frame_dur AS cpu_time
FROM android_app_vsync_delay_per_frame
JOIN all_draw_frames USING (frame_id)
JOIN distinct_frames f USING (frame_id)
JOIN slice do_frame ON (f.do_frame_id = do_frame.id);

-- CPU time of frames which don't have `android_cpu_time_per_frame` available.
-- Calculated as UI time of the frame + 5ms.
-- For Googlers: more details in go/android-performance-metrics-glossary.
CREATE PERFETTO TABLE _cpu_time_per_frame_fallback(
    -- Frame id.
    frame_id INT,
    -- Estimated cpu time.
    estimated_cpu_time INT
) AS
SELECT
    frame_id,
    ui_time + time_from_ms(5) AS estimated_cpu_time
FROM android_frames_ui_time;

CREATE PERFETTO TABLE _estimated_cpu_time_per_frame(
    frame_id INT,
    cpu_time INT
) AS
SELECT
    frame_id,
    IIF(r.cpu_time IS NULL, f.estimated_cpu_time, r.cpu_time) AS cpu_time
FROM _cpu_time_per_frame_fallback f
LEFT JOIN android_cpu_time_per_frame r USING (frame_id);

-- Aggregated stats of the frame.
--
-- For Googlers: more details in go/android-performance-metrics-glossary.
CREATE PERFETTO TABLE android_frame_stats(
    -- Frame id.
    frame_id INT,
    -- The amount by which each frame missed of hit its deadline. See
    -- `android_frames_overrun` for details.
    overrun INT,
    -- How much time did the frame take across the UI Thread + RenderThread.
    cpu_time INT,
    -- How much time did the frame's Choreographer callbacks take.
    ui_time INT,
    -- Was frame janky.
    was_jank BOOL,
    -- CPU time of the frame took over 20ms.
    was_slow_frame BOOL,
    -- CPU time of the frame took over 50ms.
    was_big_jank BOOL,
    -- CPU time of the frame took over 200ms.
    was_huge_jank BOOL
) AS
SELECT
    frame_id,
    overrun,
    cpu_time,
    ui_time,
    IIF(overrun > 0, 1, NULL) AS was_jank,
    IIF(cpu_time > time_from_ms(20), 1, NULL) AS was_slow_frame,
    IIF(cpu_time > time_from_ms(50), 1, NULL) AS was_big_jank,
    IIF(cpu_time > time_from_ms(200), 1, NULL) AS was_huge_jank
FROM android_frames_overrun
JOIN android_frames_ui_time USING (frame_id)
-- Because some frames might not have CPU time calculated properly (data loss
-- or too old API), we will use fallback cpu time from
-- `_cpu_time_per_frame_fallback`.
JOIN _estimated_cpu_time_per_frame USING (frame_id);

