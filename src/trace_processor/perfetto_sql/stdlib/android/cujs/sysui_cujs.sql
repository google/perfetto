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

INCLUDE PERFETTO MODULE android.frames.timeline;

-- Exhaustive list of jank and latency CUJs.
CREATE PERFETTO TABLE _sysui_cujs_slices AS
SELECT
  row_number() OVER (ORDER BY ts) AS cuj_id,
  process.upid AS upid,
  process.name AS process_name,
  slice.id AS slice_id,
  slice.name AS cuj_slice_name,
  ts,
  dur,
  ts + dur AS ts_end
FROM slice
JOIN process_track
  ON slice.track_id = process_track.id
JOIN process
  USING (upid)
WHERE
  slice.name GLOB 'J<*>'
  OR slice.name GLOB 'L<*>'
  AND (
    process.name GLOB 'com.google.android*' OR process.name GLOB 'com.android.*'
  )
  AND dur > 0;

-- Slices logged from FrameTracker#markEvent that describe when
-- the instrumentation was started and the reason the CUJ ended.
CREATE PERFETTO TABLE _sysui_cuj_state_markers AS
SELECT
  cuj.cuj_id,
  upid,
  CASE
    WHEN cuj_state_marker.name GLOB '*FT#begin*'
    THEN 'begin'
    WHEN cuj_state_marker.name GLOB '*FT#deferMonitoring*'
    THEN 'deferMonitoring'
    WHEN cuj_state_marker.name GLOB '*FT#end*'
    THEN 'end'
    WHEN cuj_state_marker.name GLOB '*FT#cancel*'
    THEN 'cancel'
    WHEN cuj_state_marker.name GLOB '*FT#layerId*'
    THEN 'layerId'
    WHEN cuj_state_marker.name GLOB '*#UIThread'
    THEN 'UIThread'
    ELSE 'other'
  END AS marker_type,
  cuj_state_marker.name AS marker_name,
  thread_track.utid AS utid
FROM _sysui_cujs_slices AS cuj
LEFT JOIN slice AS cuj_state_marker
  ON cuj_state_marker.ts >= cuj.ts AND cuj_state_marker.ts < cuj.ts_end
LEFT JOIN track AS marker_track
  ON marker_track.id = cuj_state_marker.track_id
LEFT JOIN thread_track
  ON cuj_state_marker.track_id = thread_track.id
WHERE
  -- e.g. J<CUJ_NAME>#FT#end#0 this for backward compatibility
  cuj_state_marker.name GLOB (
    cuj.cuj_slice_name || "#FT#*"
  )
  OR (
    marker_track.name = cuj_slice_name AND cuj_state_marker.name GLOB 'FT#*'
  )
  OR cuj_state_marker.name = (
    cuj.cuj_slice_name || "#UIThread"
  );

-- CUJ instant event values.
CREATE PERFETTO TABLE _sysui_cuj_instant_events AS
SELECT
  cuj_id,
  cuj.upid,
  (
    SELECT
      CAST(str_split(csm.marker_name, 'layerId#', 1) AS INTEGER)
    FROM _sysui_cuj_state_markers AS csm
    WHERE
      csm.cuj_id = cuj.cuj_id AND csm.marker_name GLOB '*layerId#*'
    LIMIT 1
  ) AS layer_id,
  (
    SELECT
      CAST(str_split(csm.marker_name, 'beginVsync#', 1) AS INTEGER)
    FROM _sysui_cuj_state_markers AS csm
    WHERE
      csm.cuj_id = cuj.cuj_id AND csm.marker_name GLOB '*beginVsync#*'
    LIMIT 1
  ) AS begin_vsync,
  (
    SELECT
      CAST(str_split(csm.marker_name, 'endVsync#', 1) AS INTEGER)
    FROM _sysui_cuj_state_markers AS csm
    WHERE
      csm.cuj_id = cuj.cuj_id AND csm.marker_name GLOB '*endVsync#*'
    LIMIT 1
  ) AS end_vsync,
  (
    SELECT
      utid
    FROM _sysui_cuj_state_markers AS csm
    WHERE
      csm.cuj_id = cuj.cuj_id AND csm.marker_name GLOB "*#UIThread"
    LIMIT 1
  ) AS ui_thread
FROM _sysui_cuj_state_markers
JOIN _sysui_cujs_slices AS cuj
  USING (cuj_id)
GROUP BY
  cuj_id;

-- Table tracking all jank CUJs information.
CREATE PERFETTO TABLE android_sysui_jank_cujs (
  -- Unique incremental ID for each CUJ.
  cuj_id LONG,
  -- process id.
  upid JOINID(process.id),
  -- process name.
  process_name STRING,
  -- Name of the CUJ slice.
  cuj_slice_name STRING,
  -- Name of the CUJ without the 'J<' prefix.
  cuj_name STRING,
  -- Id of the CUJ slice in perfetto. Keeping the slice id column as part of this table
  -- as provision to lookup the actual CUJ slice ts and dur. The ts and dur in this table
  -- might differ from the slice duration, as they are associated with start and end frame
  -- corresponding to the CUJ.
  slice_id JOINID(slice.id),
  -- Start timestamp of the CUJ. Start of the CUJ as defined by the start of the CUJ slice,
  -- or start of the actual frame, whichever is greater.
  ts TIMESTAMP,
  -- End timestamp of the CUJ. Calculated as the end timestamp of the last frame overlapping with the CUJ.
  ts_end TIMESTAMP,
  -- Duration of the CUJ calculated based on the ts and ts_end values.
  dur DURATION,
  -- State of the CUJ whether it was completed/cancelled.
  state STRING,
  -- thread id of the UI thread.
  ui_thread JOINID(thread.id),
  -- layer id associated with the actual frame.
  layer_id LONG,
  -- vysnc id of the first frame that falls within the CUJ boundary.
  begin_vsync LONG,
  -- vysnc id of the last frame that falls within the CUJ boundary.
  end_vsync LONG
) AS
WITH
  -- Track all frames that overlap with the CUJ slice, with the same layer id.
  frames_in_cuj AS (
    SELECT
      row_number() OVER (PARTITION BY cuj.cuj_id ORDER BY frame.ts) AS frame_idx,
      count(*) OVER (PARTITION BY cuj.cuj_id) AS frame_cnt,
      cuj.cuj_slice_name,
      cuj.upid,
      cuj.process_name,
      frame.layer_id,
      frame.frame_id,
      cuj.cuj_id,
      frame.ts AS frame_ts,
      frame.dur AS dur,
      (
        frame.ts + frame.dur
      ) AS ts_end
    FROM android_frames_layers AS frame
    JOIN _sysui_cuj_instant_events AS cie
      ON frame.layer_id = cie.layer_id AND frame.ui_thread_utid = cie.ui_thread
    JOIN _sysui_cujs_slices AS cuj
      ON cie.cuj_id = cuj.cuj_id
    -- Check whether the frame_id falls within the begin and end vsync of the cuj.
    -- Also check if the frame start or end timestamp falls within the cuj boundary.
    WHERE
      (
        -- frame start within cuj
        (
          frame.ts >= cuj.ts AND frame.ts <= cuj.ts_end
        )
        -- frame end within cuj
        OR (
          (
            frame.ts + frame.dur
          ) >= cuj.ts AND (
            frame.ts + frame.dur
          ) <= cuj.ts_end
        )
      )
  ),
  -- select the first and last frame.
  cuj_frame_boundary AS (
    SELECT
      *
    FROM frames_in_cuj
    WHERE
      frame_idx = 1 OR frame_idx = frame_cnt
  )
SELECT
  cuj.cuj_id,
  cuj.upid,
  cuj.process_name,
  cuj.cuj_slice_name,
  -- Extracts "CUJ_NAME" from "J<CUJ_NAME>"
  substr(cuj.cuj_slice_name, 3, length(cuj.cuj_slice_name) - 3) AS cuj_name,
  cuj.slice_id,
  min(boundary.frame_ts) AS ts,
  max(boundary.ts_end) AS ts_end,
  (
    max(boundary.ts_end) - min(boundary.frame_ts)
  ) AS dur,
  CASE
    WHEN EXISTS(
      SELECT
        1
      FROM _sysui_cuj_state_markers AS csm
      WHERE
        csm.cuj_id = cuj.cuj_id AND csm.marker_type = 'cancel'
    )
    THEN 'canceled'
    WHEN EXISTS(
      SELECT
        1
      FROM _sysui_cuj_state_markers AS csm
      WHERE
        csm.cuj_id = cuj.cuj_id AND csm.marker_type = 'end'
    )
    THEN 'completed'
    ELSE NULL
  END AS state,
  cuj_events.ui_thread,
  cuj_events.layer_id,
  cuj_events.begin_vsync,
  cuj_events.end_vsync
FROM _sysui_cujs_slices AS cuj
JOIN _sysui_cuj_instant_events AS cuj_events
  USING (cuj_id)
JOIN cuj_frame_boundary AS boundary
  USING (cuj_id)
WHERE
  -- Filter only jank CUJs.
  cuj.cuj_slice_name GLOB 'J<*>'
  AND (
    state != 'canceled'
    -- Older builds don't have the state markers so we allow NULL but filter out
    -- CUJs that are <4ms long - assuming CUJ was canceled in that case.
    OR (
      state IS NULL AND cuj.dur > 4e6
    )
  )
GROUP BY
  cuj_id
ORDER BY
  ts ASC;

-- Table tracking all latency CUJs information.
CREATE PERFETTO TABLE android_sysui_latency_cujs (
  -- Unique incremental ID for each CUJ.
  cuj_id LONG,
  -- process id.
  upid JOINID(process.id),
  -- process name.
  process_name STRING,
  -- Name of the CUJ slice.
  cuj_slice_name STRING,
  -- Name of the CUJ without the 'L<' prefix.
  cuj_name STRING,
  -- Id of the CUJ slice in perfetto. Keeping the slice id column as part of this table
  -- as provision to lookup the actual CUJ slice ts and dur. The ts and dur in this table
  -- might differ from the slice duration, as they are associated with start and end frame
  -- corresponding to the CUJ.
  slice_id JOINID(slice.id),
  -- Start timestamp of the CUJ. Start of the CUJ as defined by the start of the CUJ slice,
  -- or start of the actual frame, whichever is greater.
  ts TIMESTAMP,
  -- End timestamp of the CUJ. Calculated as the end timestamp of the last frame overlapping with the CUJ.
  ts_end TIMESTAMP,
  -- Duration of the CUJ calculated based on the ts and ts_end values.
  dur DURATION,
  -- State of the CUJ whether it was completed/cancelled.
  state STRING,
  -- thread id of the UI thread.
  ui_thread JOINID(thread.id)
) AS
SELECT
  cuj.cuj_id,
  cuj.upid,
  cuj.process_name,
  cuj.cuj_slice_name,
  -- Extracts "CUJ_NAME" from "L<CUJ_NAME>"
  substr(cuj.cuj_slice_name, 3, length(cuj.cuj_slice_name) - 3) AS cuj_name,
  cuj.slice_id,
  cuj.ts,
  cuj.ts_end,
  cuj.dur,
  CASE
    WHEN EXISTS(
      SELECT
        1
      FROM _sysui_cuj_state_markers AS csm
      WHERE
        csm.cuj_id = cuj.cuj_id AND csm.marker_type = 'cancel'
    )
    THEN 'canceled'
    WHEN EXISTS(
      SELECT
        1
      FROM _sysui_cuj_state_markers AS csm
      WHERE
        csm.cuj_id = cuj.cuj_id AND csm.marker_type = 'end'
    )
    THEN 'completed'
    ELSE NULL
  END AS state,
  cuj_events.ui_thread
FROM _sysui_cujs_slices AS cuj
JOIN _sysui_cuj_instant_events AS cuj_events
  USING (cuj_id)
WHERE
  -- Filter only latency CUJs.
  cuj.cuj_slice_name GLOB 'L<*>'
  AND (
    state != 'canceled'
    -- Older builds don't have the state markers so we allow NULL but filter out
    -- CUJs that are <4ms long - assuming CUJ was canceled in that case.
    OR (
      state IS NULL AND cuj.dur > 4e6
    )
  )
GROUP BY
  cuj_id
ORDER BY
  ts ASC;
