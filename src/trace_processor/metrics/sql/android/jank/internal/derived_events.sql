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


DROP VIEW IF EXISTS android_jank_cuj_event;
CREATE PERFETTO VIEW android_jank_cuj_event AS
-- Computed CUJ boundaries.
SELECT
  'slice' AS track_type,
  cuj.cuj_name AS track_name,
  boundary.ts,
  boundary.dur,
  cuj.cuj_name || ' (adjusted, id=' || cuj_id || ') ' AS slice_name,
  'CUJ Boundaries' AS group_name
FROM android_jank_cuj cuj
JOIN android_jank_cuj_boundary boundary USING (cuj_id)
UNION ALL
-- Computed frame boundaries on the Main Thread.
SELECT
  'slice' AS track_type,
  cuj.cuj_name || ' MT ' || vsync AS track_name,
  boundary.ts,
  boundary.dur,
  vsync || '' AS slice_name,
  cuj.cuj_name || ' - MT frame boundaries' AS group_name
FROM android_jank_cuj cuj
JOIN android_jank_cuj_main_thread_frame_boundary boundary USING (cuj_id)
UNION ALL
-- Computed frame boundaries on the Render Thread.
SELECT
  'slice' AS track_type,
  cuj.cuj_name || ' RT ' || vsync AS track_name,
  boundary.ts,
  boundary.dur,
  vsync || '' AS slice_name,
  cuj.cuj_name || ' - RT frame boundaries' AS group_name
FROM android_jank_cuj cuj
JOIN android_jank_cuj_render_thread_frame_boundary boundary USING (cuj_id)
UNION ALL
-- Computed overall frame boundaries not specific to any thread.
SELECT
  'slice' AS track_type,
  cuj.cuj_name || ' ' || vsync AS track_name,
  f.ts,
  f.dur,
  vsync || ' [app_missed=' || f.app_missed || ']' AS slice_name,
  cuj.cuj_name || ' - frames' AS group_name
FROM android_jank_cuj cuj
JOIN android_jank_cuj_frame f USING (cuj_id)
UNION ALL
-- Computed frame boundaries on the SF Main Thread
SELECT
  'slice' AS track_type,
  cuj.cuj_name || ' SF MT ' || vsync AS track_name,
  boundary.ts,
  boundary.dur,
  vsync || '' AS slice_name,
  cuj.cuj_name || ' - SF MT frame boundaries' AS group_name
FROM android_jank_cuj cuj
JOIN android_jank_cuj_sf_main_thread_frame_boundary boundary USING (cuj_id)
UNION ALL
-- Computed frame boundaries on the SF RenderEngine Thread.
SELECT
  'slice' AS track_type,
  cuj.cuj_name || ' SF RE ' || vsync AS track_name,
  boundary.ts,
  boundary.dur,
  vsync || '' AS slice_name,
  cuj.cuj_name || ' - SF RE frame boundaries' AS group_name
FROM android_jank_cuj cuj
JOIN android_jank_cuj_sf_render_engine_frame_boundary boundary USING (cuj_id);
