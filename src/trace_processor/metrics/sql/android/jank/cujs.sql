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

SELECT RUN_METRIC('android/process_metadata.sql');

-- Stores information about the CUJs (important UI transitions) in the trace.
DROP TABLE IF EXISTS android_jank_cuj;
CREATE TABLE android_jank_cuj AS
-- Finds slices like J<SHADE_EXPAND_COLLAPSE> which mark which frames were
-- rendered during a specific CUJ.
WITH cujs AS (
  SELECT
    ROW_NUMBER() OVER (ORDER BY ts) AS cuj_id,
    process.upid AS upid,
    process.name AS process_name,
    process_metadata.metadata AS process_metadata,
    slice.name AS cuj_slice_name,
    -- Extracts "CUJ_NAME" from "J<CUJ_NAME>"
    SUBSTR(slice.name, 3, LENGTH(slice.name) - 3) AS cuj_name,
    ts,
    dur,
    ts + dur AS ts_end
  FROM slice
  JOIN process_track
    ON slice.track_id = process_track.id
  JOIN process USING (upid)
  JOIN process_metadata USING (upid)
  WHERE
    slice.name GLOB 'J<*>'
    AND (
      process.name GLOB 'com.google.android*'
      OR process.name GLOB 'com.android.*')
    AND dur > 0
),
-- Slices logged from FrameTracker#markEvent that describe when
-- the instrumentation was started and the reason the CUJ ended.
cuj_state_markers AS (
  SELECT
    cujs.cuj_id,
    CASE
      WHEN cuj_state_marker.name GLOB '*#FT#begin*' THEN 'begin'
      WHEN cuj_state_marker.name GLOB '*#FT#deferMonitoring*' THEN 'deferMonitoring'
      WHEN cuj_state_marker.name GLOB '*#FT#end*' THEN 'end'
      WHEN cuj_state_marker.name GLOB '*#FT#cancel*' THEN 'cancel'
      ELSE 'other'
    END AS marker_type
  FROM cujs
  LEFT JOIN slice cuj_state_marker
    ON cuj_state_marker.ts >= cujs.ts
      AND cuj_state_marker.ts < cujs.ts_end
      -- e.g. J<CUJ_NAME>#FT#end#0
      AND cuj_state_marker.name GLOB (cujs.cuj_slice_name || "#FT#*")
)
SELECT
  cujs.*,
  CASE
    WHEN EXISTS (
      SELECT 1
      FROM cuj_state_markers csm
      WHERE csm.cuj_id = cujs.cuj_id
        AND csm.marker_type = 'cancel')
      THEN 'canceled'
    WHEN EXISTS (
      SELECT 1
      FROM cuj_state_markers csm
      WHERE csm.cuj_id = cujs.cuj_id
        AND csm.marker_type = 'end')
      THEN 'completed'
    ELSE NULL
  END AS state
FROM cujs
WHERE
  state != 'canceled'
  -- Older builds don't have the state markers so we allow NULL but filter out
  -- CUJs that are <4ms long - assuming CUJ was canceled in that case.
  OR (state IS NULL AND cujs.dur > 4e6)
ORDER BY ts ASC;
