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
      fr.*,
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

-- Capture blocking call duration within frames within a CUJ.
--
-- The join between blocking calls and frames is split into two branches (one
-- for the UI thread, one for the render thread) that are `UNION ALL`-ed
-- together, rather than joining on `bc.utid = frame.ui_thread_utid OR bc.utid =
-- frame.render_thread_utid`. An `OR` in a join condition forces a nested loop
-- over the full cross product; two separate equality joins can each be executed
-- efficiently. The `bc.utid != frame.ui_thread_utid` guard on the render branch
-- keeps the two branches disjoint (so no row is double counted in the rare case
-- where a frame's UI and render threads are the same), making this exactly
-- equivalent to the original `OR` join.
CREATE PERFETTO TABLE _blocking_calls_frame_cuj AS
WITH
  _clipped AS (
    SELECT
      bc.id AS slice_id,
      bc.name,
      max(frame.ts, bc.ts) AS ts,
      min(frame.ts_end, bc.raw_ts_end) AS ts_end,
      bc.is_draw_frames,
      frame.cuj_id,
      frame.cuj_name,
      frame.upid,
      frame.process_name,
      bc.utid,
      frame.frame_id,
      frame.layer_id
    FROM _android_critical_blocking_calls AS bc
    JOIN _extended_frame_boundary AS frame ON bc.utid = frame.ui_thread_utid
    WHERE
      frame.ts < frame.ts_end
      AND ((bc.raw_dur > 0
      AND bc.ts < frame.ts_end
      AND bc.raw_ts_end > frame.ts)
      OR (bc.raw_dur = 0 AND bc.ts >= frame.ts AND bc.ts < frame.ts_end))
    UNION ALL
    SELECT
      bc.id AS slice_id,
      bc.name,
      max(frame.ts, bc.ts) AS ts,
      min(frame.ts_end, bc.raw_ts_end) AS ts_end,
      bc.is_draw_frames,
      frame.cuj_id,
      frame.cuj_name,
      frame.upid,
      frame.process_name,
      bc.utid,
      frame.frame_id,
      frame.layer_id
    FROM _android_critical_blocking_calls AS bc
    JOIN _extended_frame_boundary AS frame
      ON bc.utid = frame.render_thread_utid
      AND (frame.ui_thread_utid IS NULL OR bc.utid != frame.ui_thread_utid)
    WHERE
      frame.ts < frame.ts_end
      AND ((bc.raw_dur > 0
      AND bc.ts < frame.ts_end
      AND bc.raw_ts_end > frame.ts)
      OR (bc.raw_dur = 0 AND bc.ts >= frame.ts AND bc.ts < frame.ts_end))
  ),
  _draw_frames_wait_dur AS (
    SELECT
      c.slice_id,
      c.cuj_id,
      c.frame_id,
      sum(min(w.ts + w.dur, c.ts_end) - max(w.ts, c.ts)) AS wait_dur
    FROM _clipped AS c
    JOIN _android_critical_blocking_calls_wait_for_buffer_release AS w
      ON w.utid = c.utid
    WHERE
      c.is_draw_frames = 1
      AND w.dur > 0
      AND w.ts < c.ts_end
      AND w.ts + w.dur > c.ts
    GROUP BY
      c.slice_id,
      c.cuj_id,
      c.frame_id
  )
SELECT
  c.slice_id,
  c.name,
  c.ts,
  c.ts_end - c.ts AS dur,
  c.ts_end,
  iif(
    w.wait_dur IS NOT NULL,
    max((c.ts_end - c.ts) - w.wait_dur, 0),
    c.ts_end - c.ts
  ) AS blocking_dur,
  c.cuj_id,
  c.cuj_name,
  c.upid,
  c.process_name,
  c.utid,
  c.frame_id,
  c.layer_id
FROM _clipped AS c
LEFT JOIN _draw_frames_wait_dur AS w
  ON c.is_draw_frames = 1
  AND c.slice_id = w.slice_id
  AND c.cuj_id = w.cuj_id
  AND c.frame_id = w.frame_id;
