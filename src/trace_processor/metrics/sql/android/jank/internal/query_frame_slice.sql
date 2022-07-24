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

-- For simplicity we allow `relevant_slice_table_name` to be based on any
-- of the slice tables. This table filters it down to only include slices within
-- the (broadly bounded) CUJ and on the specific process / thread.
-- Using TABLE and not VIEW as this gives better, localized error messages in cases
-- `relevant_slice_table_name` is not correct (e.g. missing cuj_id).
DROP TABLE IF EXISTS {{table_name_prefix}}_query_slice;
CREATE TABLE {{table_name_prefix}}_query_slice AS
SELECT
  slice.cuj_id,
  slice.id,
  slice.name,
  slice.ts,
  slice.dur,
  slice.ts_end
FROM {{relevant_slice_table_name}} slice
JOIN {{slice_table_name}} android_jank_cuj_slice_table
  USING (cuj_id, id);

-- Flat view of frames and slices matched and "trimmed" to each frame boundaries.
DROP VIEW IF EXISTS {{table_name_prefix}}_slice_in_frame;
CREATE VIEW {{table_name_prefix}}_slice_in_frame AS
SELECT
  frame.*,
  query_slice.id AS slice_id,
  query_slice.name AS slice_name,
  MAX(query_slice.ts, frame_boundary.ts) AS slice_ts,
  MIN(query_slice.ts_end, frame_boundary.ts_end) AS slice_ts_end,
  MIN(query_slice.ts_end, frame_boundary.ts_end) - MAX(query_slice.ts, frame_boundary.ts) AS slice_dur,
  query_slice.ts_end AS ts_end_original
FROM {{frame_table_name}} frame
-- We want to use different boundaries depending on which thread's slices the query is targetting.
JOIN {{frame_boundary_table_name}} frame_boundary USING (cuj_id, vsync)
JOIN {{table_name_prefix}}_query_slice query_slice
  ON frame_boundary.cuj_id = query_slice.cuj_id
  AND ANDROID_JANK_CUJ_SLICE_OVERLAPS(frame_boundary.ts, frame_boundary.dur, query_slice.ts, query_slice.dur);

-- Aggregated view of frames and slices overall durations within each frame boundaries.
DROP VIEW IF EXISTS {{table_name_prefix}}_slice_in_frame_agg;
CREATE VIEW {{table_name_prefix}}_slice_in_frame_agg AS
SELECT
  cuj_id,
  frame_number,
  vsync,
  dur_expected,
  app_missed,
  sf_missed,
  1.0 * SUM(slice_dur) / dur_expected AS slice_dur_div_frame_dur_expected,
  SUM(slice_dur) AS slice_dur_sum,
  MAX(slice_dur) AS slice_dur_max
FROM {{table_name_prefix}}_slice_in_frame
GROUP BY cuj_id, frame_number, vsync, dur_expected, app_missed, sf_missed;
