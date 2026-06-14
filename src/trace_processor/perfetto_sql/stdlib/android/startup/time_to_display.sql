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

INCLUDE PERFETTO MODULE android.startup.startups;

INCLUDE PERFETTO MODULE android.frames.timeline;

INCLUDE PERFETTO MODULE slices.with_context;

CREATE PERFETTO PIPELINE _startups_with_upid AS
SUBPIPELINE joined_with_processes AS (
  FROM android_startups AS s
  |> LEFT JOIN android_startup_processes AS p USING (startup_id)
  |> SELECT s.*, p.upid
)
SUBPIPELINE fallback AS (
  FROM android_startups AS s
  |> JOIN process AS p ON (p.name GLOB s.package)
  |> SELECT s.*, upid
)
FROM joined_with_processes AS j
|> LEFT JOIN fallback AS f ON (j.upid IS NULL AND j.startup_id = f.startup_id)
|> SELECT
     j.startup_id,
     j.ts,
     j.ts_end,
     j.dur,
     j.package,
     j.startup_type,
     coalesce(j.upid, f.upid) AS upid;

-- Get Time To Initial Display of the startup calculated as time between the
-- startup started and the first frame that was started by Choreographer on the
-- UI thread of the startup finished drawing.
-- TTID (https://developer.android.com/topic/performance/vitals/launch-time#time-initial)
-- Googlers: see go/android-performance-metrics-glossary for details.
CREATE PERFETTO PIPELINE _ttid MATERIALIZED AS
SUBPIPELINE frames_with_upid AS (
  FROM android_frames AS f
  |> JOIN thread AS t ON (f.ui_thread_utid = t.utid)
  |> SELECT f.*
)
-- First `DrawFrame` on Render Thread after the startup.
FROM _startups_with_upid AS s
|> JOIN frames_with_upid AS f ON (s.upid = f.upid AND s.ts <= f.ts)
|> AGGREGATE
     MIN(f.ts) AS first_ts,
     ARG_MIN(f.ts, frame_id) AS frame_id,
     ARG_MIN(f.ts, draw_frame_id) AS draw_frame_id,
     ARG_MIN(f.ts, s.ts) AS startup_ts,
     ARG_MIN(f.ts, s.upid) AS upid
   GROUP BY startup_id
|> JOIN slice ON (slice.id = draw_frame_id)
|> SELECT startup_id, frame_id, draw_frame_id, ts + dur - startup_ts AS ttid, upid;

-- Get Time To Full Display of the startup calculated as time between the
-- startup started and the first frame that was started by Choreographer after
-- or containing the `reportFullyDrawn()` slice on the UI thread of the startup
-- finished drawing.
-- TTFD (https://developer.android.com/topic/performance/vitals/launch-time#retrieve-TTFD)
-- Googlers: see go/android-performance-metrics-glossary for details.
CREATE PERFETTO PIPELINE _ttfd MATERIALIZED AS
-- First `reportFullyDrawn` slice for each startup.
SUBPIPELINE first_report_fully_drawn_for_startup AS (
  FROM _startups_with_upid AS s
  |> JOIN thread_slice AS t ON (s.upid = t.upid AND t.ts >= s.ts)
  |> WHERE name GLOB "reportFullyDrawn*" AND t.is_main_thread = 1
  |> AGGREGATE
       ARG_MIN(t.ts, s.ts) AS startup_ts,
       MIN(t.ts) AS report_fully_drawn_ts,
       ARG_MIN(t.ts, t.utid) AS utid,
       ARG_MIN(t.ts, s.upid) AS upid
     GROUP BY startup_id
)
-- After the first `reportFullyDrawn` find the first `Choreographer#DoFrame` on
-- the UI thread and it's first `DrawFrame` on Render Thread.
-- Get TTFD as the difference between the start of the startup and the end of
-- `DrawFrame` slice we previously found.
FROM first_report_fully_drawn_for_startup AS s
-- We are looking for the first DrawFrame that was started by the first
-- "Choreographer#DoFrame" on UI thread after or containing
-- reportFullyDrawn. In Android UIs, it's common to have UI code happen
-- either before a frame, or during it, and generally non-trivial amounts
-- of "update UI model" code doesn't try to differentiate these. We account
-- for both of these by looking for the first UI slice that ends after the
-- "reportFullyDrawnSlice" begins.
|> JOIN android_frames AS f
     ON (s.utid = f.ui_thread_utid AND report_fully_drawn_ts < (f.ts + f.dur))
|> AGGREGATE
     ARG_MIN(f.ts, frame_id) AS frame_id,
     ARG_MIN(f.ts, startup_ts) AS startup_ts,
     ARG_MIN(f.ts, draw_frame_id) AS draw_frame_id,
     ARG_MIN(f.ts, s.upid) AS upid
   GROUP BY startup_id
|> JOIN slice ON (slice.id = draw_frame_id)
|> SELECT startup_id, frame_id, draw_frame_id, ts + dur - startup_ts AS ttfd, upid;

-- Startup metric defintions, which focus on the observable time range:
-- TTID - Time To Initial Display
-- * https://developer.android.com/topic/performance/vitals/launch-time#time-initial
-- * end of first RenderThread.DrawFrame - bindApplication
-- TTFD - Time To Full Display
-- * https://developer.android.com/topic/performance/vitals/launch-time#retrieve-TTFD
-- * end of next RT.DrawFrame, after reportFullyDrawn called - bindApplication
-- Googlers: see go/android-performance-metrics-glossary for details.
CREATE PERFETTO PIPELINE android_startup_time_to_display(
  -- Startup id.
  startup_id LONG,
  -- Time to initial display (TTID)
  time_to_initial_display LONG,
  -- Time to full display (TTFD)
  time_to_full_display LONG,
  -- `android_frames.frame_id` of frame for initial display
  ttid_frame_id LONG,
  -- `android_frames.frame_id` of frame for full display
  ttfd_frame_id LONG,
  -- `process.upid` of the startup
  upid JOINID(process.id)
) MATERIALIZED AS
FROM android_startups
|> LEFT JOIN _ttid USING (startup_id)
|> LEFT JOIN _ttfd USING (startup_id)
|> SELECT
     startup_id,
     ttid AS time_to_initial_display,
     ttfd AS time_to_full_display,
     _ttid.frame_id AS ttid_frame_id,
     _ttfd.frame_id AS ttfd_frame_id,
     _ttid.upid;
