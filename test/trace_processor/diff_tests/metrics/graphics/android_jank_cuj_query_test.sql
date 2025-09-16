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

SELECT RUN_METRIC('android/android_jank_cuj.sql');


-- First query to look at `binder transaction` on the Main Thread

DROP VIEW IF EXISTS android_jank_cuj_query_test_binder;
CREATE VIEW android_jank_cuj_query_test_binder AS
SELECT * FROM android_jank_cuj_slice
WHERE name = 'binder transaction';

SELECT android_jank_correlate_frame_slice('MainThread', 'android_jank_cuj_query_test_binder') AS suppress_query_output;


-- Second query to look at `JIT compiling` slices on JIT threadpool

DROP VIEW IF EXISTS android_jank_cuj_query_test_jit;
CREATE VIEW android_jank_cuj_query_test_jit AS
SELECT * FROM android_jank_cuj_slice
WHERE name GLOB 'JIT compiling*';

SELECT android_jank_correlate_frame_slice_impl('App threads', 'android_jank_cuj_query_test_jit', 'jank_query_jit') AS suppress_query_output;

--- Third query to look at 'sf binder' slices on SF main thread

DROP VIEW IF EXISTS android_jank_cuj_query_test_sf_binder;
CREATE VIEW android_jank_cuj_query_test_sf_binder AS
SELECT * FROM android_jank_cuj_sf_slice
WHERE name = 'sf binder';

SELECT android_jank_correlate_frame_slice_impl('SF MainThread', 'android_jank_cuj_query_test_sf_binder', 'jank_query_sf_binder') AS suppress_query_output;


--- Fourth query to look at 'shader compile' slices on SF RenderEngine

DROP VIEW IF EXISTS android_jank_cuj_query_test_re;
CREATE VIEW android_jank_cuj_query_test_re AS
SELECT * FROM android_jank_cuj_sf_slice
WHERE name = 'shader compile';

SELECT android_jank_correlate_frame_slice_impl('SF RenderEngine', 'android_jank_cuj_query_test_re', 'jank_query_re') AS suppress_query_output;


-- UNION ALL results from all queries.
SELECT 'JIT compiling' AS slice, * FROM jank_query_jit_slice_in_frame_agg
UNION ALL
SELECT 'binder transaction' AS slice, * FROM jank_query_slice_in_frame_agg
UNION ALL
SELECT 'sf binder' AS slice, * FROM jank_query_sf_binder_slice_in_frame_agg
UNION ALL
SELECT 'shader compile' AS slice, * FROM jank_query_re_slice_in_frame_agg
ORDER BY slice, cuj_id, vsync;
