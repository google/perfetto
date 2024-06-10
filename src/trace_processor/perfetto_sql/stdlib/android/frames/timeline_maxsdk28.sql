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

-- All slices related to one frame for max SDK 28. Aggregates
-- "Choreographer#doFrame" and "DrawFrame". Tries to guess the `ts` and `dur`
-- of the frame by first guessing which "DrawFrame" slices are related to which
-- "Choreographer#doSlice".
CREATE PERFETTO TABLE _frames_maxsdk_28(
    -- Frame id. Created manually starting from 0.
    frame_id INT,
    -- Timestamp of the frame. Start of "Choreographer#doFrame" slice.
    ts INT,
    -- Duration of the frame, defined as the duration until the last
    -- "DrawFrame" of this frame finishes.
    dur INT,
    -- `slice.id` of "Choreographer#doFrame" slice.
    do_frame_id INT,
    -- `slice.id` of "DrawFrame" slice. Fetched as one of the "DrawFrame"
    -- slices that happen for the same process as "Choreographer#doFrame" slice
    -- and start after it started and before the next "doFrame" started.
    draw_frame_id INT,
    -- `utid` of the render thread.
    render_thread_utid INT,
    -- `utid` of the UI thread.
    ui_thread_utid INT,
    -- "maxsdk28"
    sdk STRING
) AS
WITH choreographer AS (
  SELECT id
  FROM slice
  WHERE name = 'Choreographer#doFrame'
),
do_frames AS (
    SELECT
        id,
        ts,
        LEAD(ts, 1, TRACE_END()) OVER (PARTITION BY upid ORDER BY ts) AS next_do_frame,
        utid,
        upid
    FROM choreographer
    JOIN thread_slice USING (id)
    WHERE is_main_thread = 1
    ORDER BY ts
),
draw_frames AS (
    SELECT
        id,
        ts,
        dur,
        ts + dur AS ts_end,
        utid,
        upid
    FROM thread_slice
    WHERE name = 'DrawFrame'
)
SELECT
  ROW_NUMBER() OVER () AS frame_id,
  do.ts,
  MAX(draw.ts_end) OVER (PARTITION BY do.id) - do.ts AS dur,
  do.id AS do_frame_id,
  draw.id AS draw_frame_id,
  draw.utid AS render_thread_utid,
  do.utid AS ui_thread_utid,
  "maxsdk28" AS sdk
FROM do_frames do
JOIN draw_frames draw ON (do.upid = draw.upid AND draw.ts >= do.ts AND draw.ts < next_do_frame)
ORDER BY do.ts;