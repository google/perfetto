--
-- Copyright 2019 The Android Open Source Project
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
--


DROP VIEW IF EXISTS {{table_name_prefix}}_main_thread;
CREATE VIEW {{table_name_prefix}}_main_thread AS
  SELECT
    process.name as process_name,
    thread.utid
  FROM thread
  JOIN {{process_allowlist_table}} process USING (upid)
  WHERE thread.is_main_thread;

DROP VIEW IF EXISTS {{table_name_prefix}}_render_thread;
CREATE VIEW {{table_name_prefix}}_render_thread AS
  SELECT
    process.name as process_name,
    thread.utid
  FROM thread
  JOIN {{process_allowlist_table}} process USING (upid)
  WHERE thread.name = 'RenderThread';

DROP VIEW IF EXISTS {{table_name_prefix}}_gpu_completion_thread;
CREATE VIEW {{table_name_prefix}}_gpu_completion_thread AS
  SELECT
    process.name as process_name,
    thread.utid
  FROM thread
  JOIN {{process_allowlist_table}} process USING (upid)
  WHERE thread.name = 'GPU completion';

DROP VIEW IF EXISTS {{table_name_prefix}}_hwc_release_thread;
CREATE VIEW {{table_name_prefix}}_hwc_release_thread AS
  SELECT
    process.name as process_name,
    thread.utid
  FROM thread
  JOIN {{process_allowlist_table}} process USING (upid)
  WHERE thread.name = 'HWC release';

DROP TABLE IF EXISTS {{table_name_prefix}}_main_thread_slices;
CREATE TABLE {{table_name_prefix}}_main_thread_slices AS
  SELECT
    process_name,
    thread.utid,
    slice.*,
    ts + dur AS ts_end
  FROM slice
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN {{table_name_prefix}}_main_thread thread USING (utid)
  WHERE dur > 0;

DROP TABLE IF EXISTS {{table_name_prefix}}_render_thread_slices;
CREATE TABLE {{table_name_prefix}}_render_thread_slices AS
  SELECT
    process_name,
    thread.utid,
    slice.*,
    ts + dur AS ts_end
  FROM slice
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN {{table_name_prefix}}_render_thread thread USING (utid)
  WHERE dur > 0;

DROP VIEW IF EXISTS {{table_name_prefix}}_gpu_completion_slices;
CREATE VIEW {{table_name_prefix}}_gpu_completion_slices AS
  SELECT
    process_name,
    thread.utid,
    slice.*,
    ts + dur AS ts_end,
    -- Extracts 1234 from 'waiting for GPU completion 1234'
    CAST(STR_SPLIT(slice.name, ' ', 4) AS INTEGER) as idx
  FROM slice
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN {{table_name_prefix}}_gpu_completion_thread thread USING (utid)
  WHERE slice.name LIKE 'waiting for GPU completion %'
  AND dur > 0;

DROP VIEW IF EXISTS {{table_name_prefix}}_hwc_release_slices;
CREATE VIEW {{table_name_prefix}}_hwc_release_slices AS
  SELECT
    process_name,
    thread.utid,
    slice.*,
    ts + dur as ts_end,
    -- Extracts 1234 from 'waiting for HWC release 1234'
    CAST(STR_SPLIT(slice.name, ' ', 4) AS INTEGER) as idx
  FROM slice
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN {{table_name_prefix}}_hwc_release_thread thread USING (utid)
  WHERE slice.name LIKE 'waiting for HWC release %'
  AND dur > 0;
