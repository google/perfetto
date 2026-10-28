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
INCLUDE PERFETTO MODULE android.binder;

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
-- Only frames belonging to completed jank CUJs (`android_sysui_jank_cujs`) are included.
CREATE PERFETTO TABLE _extended_frame_boundary AS
-- Get the start time of the NEXT frame in the sequence
WITH
  _frames_with_next AS (
    SELECT
      fr.frame_id,
      fr.frame_ts,
      fr.ts_end,
      fr.ui_thread_utid,
      fr.render_thread_utid,
      fr.cuj_id,
      fr.cuj_name,
      fr.upid,
      fr.process_name,
      fr.layer_id,
      lead(fr.frame_ts) OVER (PARTITION BY fr.cuj_id ORDER BY fr.frame_id) AS next_frame_start
    FROM _android_distinct_frames_in_cuj AS fr
    JOIN android_sysui_jank_cujs USING (cuj_id)
  ),
  _frames_with_extension_options AS (
    SELECT
      fr.frame_id,
      fr.frame_ts AS ts,
      fr.ui_thread_utid,
      fr.render_thread_utid,
      fr.cuj_id,
      fr.cuj_name,
      fr.upid,
      fr.process_name,
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
  upid,
  process_name,
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

-- Un-truncated critical blocking calls on threads participating in completed
-- jank CUJ frames, preserving raw `[ts, ts_end)` bounds so downstream interval
-- intersections cover the full slice span.
CREATE PERFETTO TABLE _cuj_raw_blocking_calls AS
WITH
  _target_threads AS (
    SELECT DISTINCT ui_thread_utid AS utid
    FROM _extended_frame_boundary
    WHERE
      ui_thread_utid IS NOT NULL
    UNION
    SELECT DISTINCT render_thread_utid AS utid
    FROM _extended_frame_boundary
    WHERE
      render_thread_utid IS NOT NULL
  ),
  _all_raw AS (
    SELECT
      s.id AS slice_id,
      s.standardized_name AS name,
      s.ts,
      s.ts + s.dur AS ts_end,
      s.utid,
      s.is_draw_frames
    FROM _android_critical_blocking_call_slices AS s
    JOIN _target_threads USING (utid)
    WHERE
      s.dur > 0
    UNION ALL
    -- Synthetic summary row across all `drawLayer *` slices without the layer name.
    SELECT
      s.id AS slice_id,
      'drawLayer' AS name,
      s.ts,
      s.ts + s.dur AS ts_end,
      s.utid,
      0 AS is_draw_frames
    FROM _android_critical_blocking_call_slices AS s
    JOIN _target_threads USING (utid)
    WHERE
      s.is_draw_layer
      AND s.dur > 0
    UNION ALL
    -- Synchronous binder transactions with AIDL names on target threads.
    SELECT
      tx.binder_txn_id AS slice_id,
      tx.aidl_name AS name,
      tx.client_ts AS ts,
      tx.client_ts + tx.client_dur AS ts_end,
      tx.client_utid AS utid,
      0 AS is_draw_frames
    FROM android_binder_txns AS tx
    JOIN _target_threads AS t ON tx.client_utid = t.utid
    WHERE
      tx.aidl_name IS NOT NULL
      AND tx.is_sync = 1
      AND tx.client_dur > 0
  )
SELECT slice_id, name, ts, ts_end, utid, is_draw_frames
FROM _all_raw
ORDER BY
  ts;

-- Frame-clipped UI-thread and RenderThread blocking call intervals.
--
-- The join between blocking calls and frames is split into two branches (one
-- for the UI thread, one for the render thread) that are `UNION ALL`-ed
-- together, rather than joining on `bc.utid = frame.ui_thread_utid OR bc.utid =
-- frame.render_thread_utid`. An `OR` in a join condition forces a nested loop
-- over the full cross product; two separate equality joins can each be executed
-- efficiently. The `bc.utid != frame.ui_thread_utid` guard on the render branch
-- keeps the two branches disjoint (so no row is double counted in the rare case
-- where a frame's UI and render threads are the same).
CREATE PERFETTO TABLE _cuj_frame_slice_blocking_calls AS
SELECT
  bc.slice_id,
  bc.name,
  max(frame.ts, bc.ts) AS ts,
  min(frame.ts_end, bc.ts_end) AS ts_end,
  bc.is_draw_frames,
  frame.cuj_id,
  frame.cuj_name,
  frame.upid,
  frame.process_name,
  bc.utid,
  frame.frame_id,
  frame.layer_id
FROM _cuj_raw_blocking_calls AS bc
JOIN _extended_frame_boundary AS frame ON bc.utid = frame.ui_thread_utid
WHERE
  frame.ts < frame.ts_end
  AND bc.ts < frame.ts_end
  AND bc.ts_end > frame.ts
UNION ALL
SELECT
  bc.slice_id,
  bc.name,
  max(frame.ts, bc.ts) AS ts,
  min(frame.ts_end, bc.ts_end) AS ts_end,
  bc.is_draw_frames,
  frame.cuj_id,
  frame.cuj_name,
  frame.upid,
  frame.process_name,
  bc.utid,
  frame.frame_id,
  frame.layer_id
FROM _cuj_raw_blocking_calls AS bc
JOIN _extended_frame_boundary AS frame
  ON bc.utid = frame.render_thread_utid
  AND (frame.ui_thread_utid IS NULL OR bc.utid != frame.ui_thread_utid)
WHERE
  frame.ts < frame.ts_end
  AND bc.ts < frame.ts_end
  AND bc.ts_end > frame.ts;

-- Frame-clipped `DrawFrames` intervals coalesced per `(cuj_id, frame_id)` so
-- multi-layer frames with overlapping `DrawFrames` slices never double-count
-- `hwuiTask` running intervals.
CREATE PERFETTO TABLE _cuj_coalesced_draw_frames AS
WITH
  _clipped_draw_frames AS (
    SELECT
      frame.cuj_id,
      frame.cuj_name,
      frame.upid,
      frame.process_name,
      frame.frame_id,
      frame.layer_id,
      max(frame.ts, s.ts) AS ts,
      min(frame.ts_end, s.ts + s.dur) AS ts_end
    FROM _extended_frame_boundary AS frame
    JOIN android_frames_draw_frame AS df
      ON df.frame_id = frame.frame_id
      AND df.render_thread_utid = frame.render_thread_utid
      AND df.upid = frame.upid
    JOIN slice AS s ON s.id = df.id
    WHERE
      frame.ts < frame.ts_end
      AND s.dur > 0
      AND s.ts < frame.ts_end
      AND s.ts + s.dur > frame.ts
  ),
  _with_prev_end AS (
    SELECT
      *,
      max(ts_end) OVER (
        PARTITION BY
          cuj_id,
          frame_id
        ORDER BY ts, ts_end
        ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
      ) AS prev_max_ts_end
    FROM _clipped_draw_frames
  ),
  _with_islands AS (
    SELECT
      *,
      sum(iif(prev_max_ts_end IS NULL OR ts > prev_max_ts_end, 1, 0)) OVER (
        PARTITION BY
          cuj_id,
          frame_id
        ORDER BY ts, ts_end
        ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
      ) AS island_id
    FROM _with_prev_end
  )
SELECT
  cuj_id,
  cuj_name,
  upid,
  process_name,
  frame_id,
  layer_id,
  min(ts) AS ts,
  max(ts_end) AS ts_end
FROM _with_islands
GROUP BY
  cuj_id,
  frame_id,
  island_id
ORDER BY
  ts;

-- Running intervals of `hwuiTask*` worker threads belonging to processes with
-- completed jank CUJ frames. Materialized once so the overlap join with
-- `_cuj_coalesced_draw_frames` avoids rescanning `thread_state` per frame.
CREATE PERFETTO TABLE _cuj_hwui_task_running AS
SELECT t.upid, ts.utid, ts.ts, ts.ts + ts.dur AS ts_end
FROM (
  SELECT DISTINCT t.upid, t.utid
  FROM (SELECT DISTINCT upid FROM _cuj_coalesced_draw_frames) AS cuj_procs
  JOIN thread AS t USING (upid)
  WHERE
    t.name GLOB 'hwuiTask*'
) AS t
JOIN thread_state AS ts ON ts.utid = t.utid
WHERE
  ts.state = 'Running'
  AND ts.dur > 0
ORDER BY
  ts.ts;

-- Per-frame blocking call intervals within completed Android Jank CUJs.
--
-- Combines UI-thread and RenderThread critical blocking call slices (including
-- synchronous AIDL binder transactions and `DrawFrames`) with `hwuiTask*`
-- worker thread `Running` intervals during each frame's `DrawFrames` slices.
--
-- Intervals (`[ts, ts_end)`, `dur = ts_end - ts > 0`) are clipped to extended
-- frame boundaries without truncating `DrawFrames` by `waitForBufferRelease`,
-- making this table safe for `_interval_intersect!` (e.g. CPU instruction or
-- scheduling attribution). Net wall-clock blocking duration (with overlapping
-- `waitForBufferRelease` time deducted on `DrawFrames`) is exposed separately
-- in `blocking_dur`.
CREATE PERFETTO TABLE android_jank_cuj_frame_blocking_calls(
  -- Unique row ID with INTEGER affinity for interval intersection joins.
  id LONG,
  -- Slice ID of the blocking call (NULL for synthetic hwuiTask rows).
  slice_id JOINID(slice.id),
  -- Standardized name of the blocking call ('hwuiTask' for hwuiTask rows).
  name STRING,
  -- Start timestamp of the frame-clipped blocking call interval.
  ts TIMESTAMP,
  -- Un-truncated duration (ts_end - ts) of the frame-clipped interval.
  dur DURATION,
  -- End timestamp (ts + dur) of the frame-clipped blocking call interval.
  ts_end TIMESTAMP,
  -- Net blocking duration (dur minus overlapping waitForBufferRelease on DrawFrames).
  blocking_dur DURATION,
  -- Completed Jank CUJ ID from android_sysui_jank_cujs.
  cuj_id LONG,
  -- Name of the Jank CUJ.
  cuj_name STRING,
  -- Process ID of the CUJ target process.
  upid JOINID(process.id),
  -- Process name of the CUJ target process.
  process_name STRING,
  -- Thread ID executing the work (UI/RenderThread for slices, hwuiTask* thread for hwuiTask).
  utid JOINID(thread.id),
  -- Frame VSync ID.
  frame_id LONG,
  -- SurfaceFlinger layer ID of the frame.
  layer_id LONG,
  -- Whether this row represents hwuiTask running state (1) or a slice blocking call (0).
  is_hwui_task BOOL
)
AS
WITH
  _draw_frames_wait_dur AS (
    SELECT
      bc.slice_id,
      bc.cuj_id,
      bc.frame_id,
      sum(min(w.ts + w.dur, bc.ts_end) - max(w.ts, bc.ts)) AS wait_dur
    FROM _cuj_frame_slice_blocking_calls AS bc
    JOIN _android_critical_blocking_calls_wait_for_buffer_release AS w
      ON w.utid = bc.utid
    WHERE
      bc.is_draw_frames = 1
      AND w.dur > 0
      AND w.ts < bc.ts_end
      AND w.ts + w.dur > bc.ts
    GROUP BY
      bc.slice_id,
      bc.cuj_id,
      bc.frame_id
  ),
  _all_frame_blocking_calls AS (
    SELECT
      bc.slice_id,
      bc.name,
      bc.ts,
      bc.ts_end - bc.ts AS dur,
      bc.ts_end,
      iif(
        bc.is_draw_frames = 1
        AND w.wait_dur IS NOT NULL,
        max((bc.ts_end - bc.ts) - w.wait_dur, 0),
        bc.ts_end - bc.ts
      ) AS blocking_dur,
      bc.cuj_id,
      bc.cuj_name,
      bc.upid,
      bc.process_name,
      bc.utid,
      bc.frame_id,
      bc.layer_id,
      0 AS is_hwui_task
    FROM _cuj_frame_slice_blocking_calls AS bc
    LEFT JOIN _draw_frames_wait_dur AS w
      ON bc.is_draw_frames = 1
      AND bc.slice_id = w.slice_id
      AND bc.cuj_id = w.cuj_id
      AND bc.frame_id = w.frame_id
    UNION ALL
    SELECT
      CAST(NULL AS INTEGER) AS slice_id,
      'hwuiTask' AS name,
      max(df.ts, r.ts) AS ts,
      min(df.ts_end, r.ts_end) - max(df.ts, r.ts) AS dur,
      min(df.ts_end, r.ts_end) AS ts_end,
      min(df.ts_end, r.ts_end) - max(df.ts, r.ts) AS blocking_dur,
      df.cuj_id,
      df.cuj_name,
      df.upid,
      df.process_name,
      r.utid,
      df.frame_id,
      df.layer_id,
      1 AS is_hwui_task
    FROM _cuj_coalesced_draw_frames AS df
    JOIN _cuj_hwui_task_running AS r ON r.upid = df.upid
    WHERE
      r.ts < df.ts_end
      AND r.ts_end > df.ts
  )
SELECT
  CAST(row_number() OVER (
    ORDER BY ts, utid, is_hwui_task, name, cuj_id, frame_id, slice_id, ts_end
  ) AS INTEGER) AS id,
  slice_id,
  name,
  ts,
  dur,
  ts_end,
  blocking_dur,
  cuj_id,
  cuj_name,
  upid,
  process_name,
  utid,
  frame_id,
  layer_id,
  is_hwui_task
FROM _all_frame_blocking_calls
ORDER BY
  id;

-- Legacy internal view retaining the historical `_blocking_calls_frame_cuj`
-- shape (net `blocking_dur` as `dur`, excluding `hwuiTask` rows).
CREATE PERFETTO VIEW _blocking_calls_frame_cuj AS
SELECT
  blocking_dur AS dur,
  ts,
  upid,
  name,
  process_name,
  utid,
  frame_id,
  layer_id,
  cuj_id,
  cuj_name
FROM android_jank_cuj_frame_blocking_calls
WHERE
  NOT is_hwui_task;
