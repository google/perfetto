--
-- Copyright 2021 The Android Open Source Project
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

DROP VIEW IF EXISTS {{table_name_prefix}}_relevant_slices_in_cuj;
CREATE VIEW {{table_name_prefix}}_relevant_slices_in_cuj AS
SELECT slice.ts, slice.dur FROM {{relevant_slices_table}} slice
JOIN android_sysui_cuj_ts_boundaries boundaries
ON slice.ts + slice.dur >= boundaries.ts AND slice.ts <= boundaries.ts_end;

DROP TABLE IF EXISTS {{table_name_prefix}}_cuj_join_table;
CREATE VIRTUAL TABLE {{table_name_prefix}}_cuj_join_table
USING span_join(
  android_sysui_cuj_ts_boundaries,
  {{table_name_prefix}}_relevant_slices_in_cuj);

DROP TABLE IF EXISTS {{table_name_prefix}}_frame_join_table;
CREATE VIRTUAL TABLE {{table_name_prefix}}_frame_join_table
USING span_join(
  android_sysui_cuj_missed_frames_hwui_times partitioned frame_number,
  {{table_name_prefix}}_relevant_slices_in_cuj);

DROP VIEW IF EXISTS {{table_name_prefix}}_per_cuj_output_data;
CREATE VIEW {{table_name_prefix}}_per_cuj_output_data AS
SELECT SUM(dur) as dur_sum, MAX(dur) as dur_max
FROM {{table_name_prefix}}_cuj_join_table;

DROP VIEW IF EXISTS {{table_name_prefix}}_per_frame_output_data;
CREATE VIEW {{table_name_prefix}}_per_frame_output_data AS
SELECT
f.frame_number,
f.vsync,
f.dur_frame,
f.app_missed,
SUM(jt.dur) as dur_sum,
MAX(jt.dur) as dur_max
FROM android_sysui_cuj_missed_frames f
JOIN {{table_name_prefix}}_frame_join_table jt USING (frame_number)
GROUP BY f.frame_number, f.vsync, f.dur_frame, f.app_missed;
