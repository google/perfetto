--
-- Copyright 2025 The Android Open Source Project
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

-- This module primarily performs composition between the blocking calls, frames and CUJs.
-- This is used for capturing blocking call per frame metrics, and the related plugins.
INCLUDE PERFETTO MODULE android.critical_blocking_calls;

INCLUDE PERFETTO MODULE android.cujs.base;

INCLUDE PERFETTO MODULE android.cujs.sysui_cujs;

INCLUDE PERFETTO MODULE android.frames.timeline;

-- For cases when a blocking call starts within a frame, but does not end before the actual frame
-- ends, a part of the blocking call can be missed while calculating the metric. To avoid this
-- issue, the frame boundary is extended, spanning from the start of the current actual frame, till
-- the start of either next actual frame or the next doFrame slice - whichever is earliest;
-- we are relying on both types of slices to get more stable metrics for cases where, for instance,
-- doFrame blocks are running without any visible changes on the screen - leading to
-- no actual frames being drawn. When this happens there might be gaps in actual frame timeline,
-- so then the next doFrame would be resolved as an earlier boundary for an extended frame.
CREATE PERFETTO TABLE _extended_frame_boundary AS
-- Get the start time of the NEXT frame in the sequence
WITH
  _frames_with_next AS (
    SELECT
      *,
      lead(frame_ts) OVER (PARTITION BY cuj_id ORDER BY frame_id) AS next_frame_start
    FROM _android_distinct_frames_in_cuj
  ),
  _frames_with_extension_options AS (
    SELECT
      fr.frame_id,
      fr.frame_ts AS ts,
      fr.ui_thread_utid,
      fr.render_thread_utid,
      fr.cuj_id,
      fr.cuj_name,
      fr.layer_id,
      -- Calculate the first doFrame start that occurs AFTER the current frame ends
      (
        SELECT min(ts)
        FROM _android_jank_cuj_do_frames
        WHERE
          utid = fr.ui_thread_utid
          AND ts >= fr.ts_end
          AND ts <= fr.next_frame_start
      ) AS next_do_slice_after_frame,
      fr.next_frame_start,
      fr.ts_end AS original_ts_end
    FROM _frames_with_next AS fr
  )
SELECT
  ts,
  ui_thread_utid,
  render_thread_utid,
  frame_id,
  layer_id,
  cuj_id,
  cuj_name,
  -- Earliest of (Next Frame Start, Next doFrame Start).
  -- Fallback to original ts_end only if both are NULL.
  coalesce(
    min(next_do_slice_after_frame, next_frame_start),
    next_do_slice_after_frame,
    next_frame_start,
    original_ts_end
  ) AS ts_end
FROM _frames_with_extension_options
ORDER BY
  frame_id;

-- Extended frame boundaries for distinct frames within completed jank CUJs.
--
-- Each frame starts at the actual frame start timestamp and extends until the
-- earliest of the next actual frame start or the next `Choreographer#doFrame`
-- start in the CUJ, falling back to the actual frame end on the final frame.
CREATE PERFETTO TABLE android_cuj_frame_boundaries(
  -- Unique incremental ID for each CUJ.
  cuj_id LONG,
  -- Name of the CUJ without the 'J<' prefix.
  cuj_name STRING,
  -- Process upid.
  upid JOINID(process.id),
  -- Process name.
  process_name STRING,
  -- Frame (vsync) ID.
  frame_id LONG,
  -- Layer ID associated with the frame.
  layer_id LONG,
  -- UI thread utid.
  ui_thread_utid JOINID(thread.id),
  -- RenderThread utid.
  render_thread_utid JOINID(thread.id),
  -- Start timestamp of the frame.
  ts TIMESTAMP,
  -- Extended end timestamp of the frame.
  ts_end TIMESTAMP,
  -- Duration of the extended frame boundary.
  dur DURATION
)
AS
SELECT
  cuj.cuj_id,
  cuj.cuj_name,
  cuj.upid,
  cuj.process_name,
  frame.frame_id,
  frame.layer_id,
  frame.ui_thread_utid,
  frame.render_thread_utid,
  frame.ts,
  frame.ts_end,
  frame.ts_end - frame.ts AS dur
FROM _extended_frame_boundary AS frame
JOIN android_sysui_jank_cujs AS cuj USING (cuj_id)
ORDER BY
  cuj.cuj_id,
  frame.frame_id;

-- `DrawFrames` slices on RenderThread for each frame in
-- `android_cuj_frame_boundaries`. Materialized so that joining `android_frames_draw_frame`
-- by integer `frame_id` and overlapping against `thread_state` for `hwuiTask`
-- executes once.
CREATE PERFETTO TABLE _cuj_frame_draw_frames AS
SELECT
  frame.cuj_id,
  frame.cuj_name,
  frame.upid,
  frame.process_name,
  frame.frame_id,
  frame.layer_id,
  s.ts,
  s.ts + s.dur AS ts_end
FROM android_cuj_frame_boundaries AS frame
JOIN android_frames_draw_frame AS df
  ON df.frame_id = frame.frame_id
  AND df.render_thread_utid = frame.render_thread_utid
  AND df.upid = frame.upid
JOIN slice AS s ON s.id = df.id
WHERE
  s.dur > 0;

-- Blocking call intervals within extended frame boundaries of completed jank
-- CUJs, including UI thread critical blocking calls, RenderThread critical
-- blocking calls, and `hwuiTask` running intervals during `DrawFrames`.
--
-- The join between critical blocking calls and frames is split into two
-- branches (UI thread and RenderThread) with `UNION ALL` instead of an `OR`
-- join so SQLite can use indexed equality joins on `utid`, while
-- `bc.utid != frame.ui_thread_utid` keeps the branches disjoint.
CREATE PERFETTO TABLE android_cuj_frame_blocking_calls(
  -- Unique row ID suitable for interval intersection.
  id LONG,
  -- Logical blocking call ID. Each critical blocking call slice in a frame has
  -- a distinct `call_id`, while all `hwuiTask` running intervals within the
  -- same `(cuj_id, frame_id)` share one `call_id` so `COUNT(DISTINCT call_id)`
  -- counts `hwuiTask` once per frame.
  call_id LONG,
  -- Standardized name of the blocking call (or 'hwuiTask').
  name STRING,
  -- Start timestamp of the blocking call interval, clamped to the frame start.
  ts TIMESTAMP,
  -- Duration of the blocking call interval within the frame boundaries.
  dur DURATION,
  -- End timestamp of the blocking call interval, clamped to the frame end.
  ts_end TIMESTAMP,
  -- Unique incremental ID for each CUJ.
  cuj_id LONG,
  -- Name of the CUJ without the 'J<' prefix.
  cuj_name STRING,
  -- Frame (vsync) ID.
  frame_id LONG,
  -- Layer ID associated with the frame.
  layer_id LONG,
  -- Process upid.
  upid JOINID(process.id),
  -- Process name.
  process_name STRING,
  -- Thread utid on which the blocking call interval ran.
  utid JOINID(thread.id)
)
AS
WITH
  _raw_slice_blocking_calls AS (
    SELECT
      bc.name,
      max(frame.ts, bc.ts) AS ts,
      min(bc.ts_end, frame.ts_end) - max(frame.ts, bc.ts) AS dur,
      min(bc.ts_end, frame.ts_end) AS ts_end,
      frame.cuj_id,
      frame.cuj_name,
      frame.frame_id,
      frame.layer_id,
      bc.upid,
      bc.process_name,
      bc.utid
    FROM _android_critical_blocking_calls AS bc
    JOIN android_cuj_frame_boundaries AS frame ON bc.utid = frame.ui_thread_utid
    WHERE
      bc.dur > 0
      AND bc.utid IS NOT NULL
      AND bc.ts < frame.ts_end
      AND bc.ts_end > frame.ts
    UNION ALL
    SELECT
      bc.name,
      max(frame.ts, bc.ts) AS ts,
      min(bc.ts_end, frame.ts_end) - max(frame.ts, bc.ts) AS dur,
      min(bc.ts_end, frame.ts_end) AS ts_end,
      frame.cuj_id,
      frame.cuj_name,
      frame.frame_id,
      frame.layer_id,
      bc.upid,
      bc.process_name,
      bc.utid
    FROM _android_critical_blocking_calls AS bc
    JOIN android_cuj_frame_boundaries AS frame
      ON bc.utid = frame.render_thread_utid
      AND bc.utid != frame.ui_thread_utid
    WHERE
      bc.dur > 0
      AND bc.utid IS NOT NULL
      AND bc.ts < frame.ts_end
      AND bc.ts_end > frame.ts
  ),
  _all_frame_blocking_calls AS (
    SELECT
      row_number() OVER (ORDER BY cuj_id, frame_id, ts, dur, utid, name) AS slice_seq,
      name,
      ts,
      dur,
      ts_end,
      cuj_id,
      cuj_name,
      frame_id,
      layer_id,
      upid,
      process_name,
      utid
    FROM _raw_slice_blocking_calls
    UNION ALL
    SELECT
      0 AS slice_seq,
      'hwuiTask' AS name,
      max(ts.ts, df.ts) AS ts,
      min(ts.ts + ts.dur, df.ts_end) - max(ts.ts, df.ts) AS dur,
      min(ts.ts + ts.dur, df.ts_end) AS ts_end,
      df.cuj_id,
      df.cuj_name,
      df.frame_id,
      df.layer_id,
      df.upid,
      df.process_name,
      t.utid
    FROM _cuj_frame_draw_frames AS df
    JOIN thread AS t ON t.upid = df.upid AND t.name GLOB 'hwuiTask*'
    JOIN thread_state AS ts ON ts.utid = t.utid AND ts.state = 'Running'
    WHERE
      ts.dur > 0
      AND ts.ts < df.ts_end
      AND ts.ts + ts.dur > df.ts
  )
SELECT
  row_number() OVER (ORDER BY cuj_id, frame_id, ts, dur, utid, name) AS id,
  dense_rank() OVER (ORDER BY cuj_id, frame_id, name, slice_seq) AS call_id,
  name,
  ts,
  dur,
  ts_end,
  cuj_id,
  cuj_name,
  frame_id,
  layer_id,
  upid,
  process_name,
  utid
FROM _all_frame_blocking_calls
ORDER BY
  id;

-- Retained for backwards compatibility. Prefer `android_cuj_frame_blocking_calls`.
CREATE PERFETTO VIEW _blocking_calls_frame_cuj AS
SELECT
  dur,
  ts,
  upid,
  name,
  process_name,
  utid,
  frame_id,
  layer_id,
  cuj_id,
  cuj_name
FROM android_cuj_frame_blocking_calls
WHERE
  name != 'hwuiTask';
