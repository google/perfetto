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

INCLUDE PERFETTO MODULE slices.with_context;
INCLUDE PERFETTO MODULE android.frames.timeline_maxsdk28;

-- Parses the slice name to fetch `frame_id` from `slice` table.
-- Use with caution. Slice names are a flaky source of ids and the resulting
-- table might require some further operations.
CREATE PERFETTO FUNCTION _get_frame_table_with_id(
    -- String just before id.
    glob_str STRING
) RETURNS TABLE (
    -- `slice.id` of the frame slice.
    id INT,
    -- Parsed frame id.
    frame_id INT,
    -- Utid.
    utid INT,
    -- Upid.
    upid INT,
    -- Timestamp of the frame slice.
    ts INT
) AS
WITH all_found AS (
    SELECT
        id,
        cast_int!(STR_SPLIT(name, ' ', 1)) AS frame_id,
        utid,
        upid,
        ts
    FROM thread_slice
    -- Mostly the frame slice is at depth 0. Though it could be pushed to depth 1 while users
    -- enable low layer trace e.g. atrace_app.
    WHERE name GLOB $glob_str AND depth IN (0, 1)
)
SELECT *
FROM all_found
-- Casting string to int returns 0 if the string can't be cast.
WHERE frame_id != 0;

-- All of the `Choreographer#doFrame` slices with their frame id.
CREATE PERFETTO TABLE android_frames_choreographer_do_frame(
    -- `slice.id`
    id INT,
    -- Frame id
    frame_id INT,
    -- Utid of the UI thread
    ui_thread_utid INT,
    -- Upid of application process
    upid INT,
    -- Timestamp of the slice.
    ts INT
) AS
SELECT
    id,
    frame_id,
    utid AS ui_thread_utid,
    upid,
    ts
-- Some OEMs have customized `doFrame` to add more information, but we've only
-- observed it added after the frame ID (b/303823815).
FROM _get_frame_table_with_id('Choreographer#doFrame*');

-- All of the `DrawFrame` slices with their frame id and render thread.
-- There might be multiple DrawFrames slices for a single vsync (frame id).
-- This happens when we are drawing multiple layers (e.g. status bar and
-- notifications).
CREATE PERFETTO TABLE android_frames_draw_frame(
    -- `slice.id`
    id INT,
    -- Frame id
    frame_id INT,
    -- Utid of the render thread
    render_thread_utid INT,
    -- Upid of application process
    upid INT
) AS
SELECT
    id,
    frame_id,
    utid AS render_thread_utid,
    upid
FROM _get_frame_table_with_id('DrawFrame*');

-- `actual_frame_timeline_slice` returns the same slice on different tracks.
-- We are getting the first slice with one frame id.
CREATE PERFETTO TABLE _distinct_from_actual_timeline_slice AS
SELECT
    cast_int!(name) AS frame_id,
    MIN(id) AS id,
    MIN(ts) AS ts,
    MAX(dur) AS dur,
    MAX(ts + dur) AS ts_end,
    count() AS count
FROM actual_frame_timeline_slice
GROUP BY 1;

-- `expected_frame_timeline_slice` returns the same slice on different tracks.
-- We are getting the first slice with one frame id.
CREATE PERFETTO TABLE _distinct_from_expected_timeline_slice AS
SELECT
    cast_int!(name) AS frame_id,
    id,
    count() AS count
FROM expected_frame_timeline_slice
GROUP BY 1;

-- All slices related to one frame. Aggregates `Choreographer#doFrame`,
-- `DrawFrame`, `actual_frame_timeline_slice` and
-- `expected_frame_timeline_slice` slices.
-- See https://perfetto.dev/docs/data-sources/frametimeline for details.
CREATE PERFETTO TABLE android_frames(
    -- Frame id.
    frame_id INT,
    -- Timestamp of the frame. Start of the frame as defined by the start of
    -- "Choreographer#doFrame" slice and the same as the start of the frame in
    -- `actual_frame_timeline_slice if present.
    ts INT,
    -- Duration of the frame, as defined by the duration of the corresponding
    -- `actual_frame_timeline_slice` or, if not present the time between the
    -- `ts` and the end of the final `DrawFrame`.
    dur INT,
    -- `slice.id` of "Choreographer#doFrame" slice.
    do_frame_id INT,
    -- `slice.id` of "DrawFrame" slice.
    draw_frame_id INT,
    -- `slice.id` from `actual_frame_timeline_slice`
    actual_frame_timeline_id INT,
    -- `slice.id` from `expected_frame_timeline_slice`
    expected_frame_timeline_id INT,
    -- `utid` of the render thread.
    render_thread_utid INT,
    -- `utid` of the UI thread.
    ui_thread_utid INT,
    -- Count of slices in `actual_frame_timeline_slice` related to this frame.
    actual_frame_timeline_count INT,
    -- Count of slices in `expected_frame_timeline_slice` related to this frame.
    expected_frame_timeline_count INT
) AS
WITH fallback AS MATERIALIZED (
    SELECT
        frame_id,
        do_frame_slice.ts AS ts,
        MAX(draw_frame_slice.ts + draw_frame_slice.dur) - do_frame_slice.ts AS dur
    FROM android_frames_choreographer_do_frame do_frame
    JOIN android_frames_draw_frame draw_frame USING (frame_id, upid)
    JOIN slice do_frame_slice ON (do_frame.id = do_frame_slice.id)
    JOIN slice draw_frame_slice ON (draw_frame.id = draw_frame_slice.id)
GROUP BY 1
),
frames_sdk_after_28 AS (
SELECT
    frame_id,
    COALESCE(act.ts, fallback.ts) AS ts,
    COALESCE(act.dur, fallback.dur) AS dur,
    do_frame.id AS do_frame_id,
    draw_frame.id AS draw_frame_id,
    draw_frame.render_thread_utid,
    do_frame.ui_thread_utid,
    "after_28" AS sdk,
    act.id AS actual_frame_timeline_id,
    exp.id AS expected_frame_timeline_id,
    act.count AS actual_frame_timeline_count,
    exp.count AS expected_frame_timeline_count
FROM android_frames_choreographer_do_frame do_frame
JOIN android_frames_draw_frame draw_frame USING (frame_id, upid)
JOIN fallback USING (frame_id)
LEFT JOIN _distinct_from_actual_timeline_slice act USING (frame_id)
LEFT JOIN _distinct_from_expected_timeline_slice exp USING (frame_id)
ORDER BY frame_id
),
all_frames AS (
    SELECT * FROM frames_sdk_after_28
    UNION
    SELECT
        *,
        NULL AS actual_frame_timeline_id,
        NULL AS expected_frame_timeline_id,
        NULL AS actual_frame_timeline_count,
        NULL AS expected_frame_timeline_count
    FROM _frames_maxsdk_28
)
SELECT
    frame_id,
    ts,
    dur,
    do_frame_id,
    draw_frame_id,
    actual_frame_timeline_id,
    expected_frame_timeline_id,
    render_thread_utid,
    ui_thread_utid,
    actual_frame_timeline_count,
    expected_frame_timeline_count
FROM all_frames
WHERE sdk = IIF(
    (SELECT COUNT(1) FROM actual_frame_timeline_slice) > 0,
    "after_28", "maxsdk28");

-- Returns first frame after the provided timestamp. The returning table has at
-- most one row.
CREATE PERFETTO FUNCTION android_first_frame_after(
    -- Timestamp.
    ts INT)
RETURNS TABLE (
    -- Frame id.
    frame_id INT,
    -- Start of the frame, the timestamp of the "Choreographer#doFrame" slice.
    ts INT,
    -- Duration of the frame.
    dur INT,
    -- `slice.id` of "Choreographer#doFrame" slice.
    do_frame_id INT,
    -- `slice.id` of "DrawFrame" slice.
    draw_frame_id INT,
    -- `slice.id` from `actual_frame_timeline_slice`
    actual_frame_timeline_id INT,
    -- `slice.id` from `expected_frame_timeline_slice`
    expected_frame_timeline_id INT,
    -- `utid` of the render thread.
    render_thread_utid INT,
    -- `utid` of the UI thread.
    ui_thread_utid INT
) AS
SELECT
    frame_id,
    ts,
    dur,
    do_frame_id,
    draw_frame_id,
    actual_frame_timeline_id,
    expected_frame_timeline_id,
    render_thread_utid,
    ui_thread_utid
FROM android_frames
WHERE ts > $ts
ORDER BY ts
LIMIT 1;
