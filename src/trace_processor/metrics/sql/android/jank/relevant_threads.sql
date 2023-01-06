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

DROP TABLE IF EXISTS android_jank_cuj_main_thread;
CREATE TABLE android_jank_cuj_main_thread AS
SELECT cuj_id, cuj.upid, utid, thread.name, thread_track.id AS track_id
FROM thread
JOIN android_jank_cuj cuj USING (upid)
JOIN thread_track USING (utid)
JOIN android_jank_cuj_param p USING (cuj_id)
WHERE
  (p.main_thread_override IS NULL AND thread.is_main_thread)
  -- Some CUJs use a dedicated thread for Choreographer callbacks
  OR (p.main_thread_override = thread.name);

SELECT CREATE_VIEW_FUNCTION(
  'ANDROID_JANK_CUJ_APP_THREAD(thread_name STRING)',
  'cuj_id INT, upid INT, utid INT, name STRING, track_id INT',
  '
  SELECT
    cuj_id,
    cuj.upid,
    utid,
    thread.name,
    thread_track.id AS track_id
  FROM thread
  JOIN android_jank_cuj cuj USING (upid)
  JOIN thread_track USING (utid)
  WHERE thread.name = $thread_name;
  '
);

DROP TABLE IF EXISTS android_jank_cuj_render_thread;
CREATE TABLE android_jank_cuj_render_thread AS
SELECT * FROM ANDROID_JANK_CUJ_APP_THREAD('RenderThread');

DROP TABLE IF EXISTS android_jank_cuj_gpu_completion_thread;
CREATE TABLE android_jank_cuj_gpu_completion_thread AS
SELECT * FROM ANDROID_JANK_CUJ_APP_THREAD('GPU completion');

DROP TABLE IF EXISTS android_jank_cuj_hwc_release_thread;
CREATE TABLE android_jank_cuj_hwc_release_thread AS
SELECT * FROM ANDROID_JANK_CUJ_APP_THREAD('HWC release');

DROP TABLE IF EXISTS android_jank_cuj_sf_process;
CREATE TABLE android_jank_cuj_sf_process AS
SELECT * FROM process
WHERE process.name = '/system/bin/surfaceflinger'
LIMIT 1;

DROP TABLE IF EXISTS android_jank_cuj_sf_main_thread;
CREATE TABLE android_jank_cuj_sf_main_thread AS
SELECT upid, utid, thread.name, thread_track.id AS track_id
FROM thread
JOIN android_jank_cuj_sf_process sf_process USING (upid)
JOIN thread_track USING (utid)
WHERE thread.is_main_thread;

SELECT CREATE_VIEW_FUNCTION(
  'ANDROID_JANK_CUJ_SF_THREAD(thread_name STRING)',
  'upid INT, utid INT, name STRING, track_id INT',
  '
  SELECT upid, utid, thread.name, thread_track.id AS track_id
  FROM thread
  JOIN android_jank_cuj_sf_process sf_process USING (upid)
  JOIN thread_track USING (utid)
  WHERE thread.name = $thread_name;
  '
);

DROP TABLE IF EXISTS android_jank_cuj_sf_gpu_completion_thread;
CREATE TABLE android_jank_cuj_sf_gpu_completion_thread AS
SELECT * FROM ANDROID_JANK_CUJ_SF_THREAD('GPU completion');

DROP TABLE IF EXISTS android_jank_cuj_sf_render_engine_thread;
CREATE TABLE android_jank_cuj_sf_render_engine_thread AS
SELECT * FROM ANDROID_JANK_CUJ_SF_THREAD('RenderEngine');
