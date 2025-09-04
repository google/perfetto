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

-- Create the base table (`android_jank_cuj`) containing all completed CUJs
-- found in the trace.
SELECT RUN_METRIC('android/jank/cujs.sql');
-- Create tables to store each CUJs main, render, HWC release,
-- and GPU completion threads.
-- Also stores the (not CUJ-specific) threads of SF: main, render engine,
-- and GPU completion threads.
SELECT RUN_METRIC('android/jank/relevant_threads.sql');

-- Create tables to store the main slices on each of the relevant threads
-- * `Choreographer#doFrame` on the main thread
-- * `DrawFrames on the render` thread
-- * `waiting for HWC release` on the HWC release thread
-- * `Waiting for GPU completion` on the GPU completion thread
-- * `commit` and `composite` on SF main thread.
-- * `REThreaded::drawLayers` on SF RenderEngine thread.
-- Also extracts vsync ids and GPU completion fence ids that allow us to match
-- slices to concrete vsync IDs.
-- Slices and vsyncs are matched between the app and SF processes by looking
-- at the actual frame timeline data.
-- We only store the slices that were produced for the vsyncs within the
-- CUJ markers.
SELECT RUN_METRIC('android/jank/relevant_slices.sql');

-- Computes the boundaries of specific frames and overall CUJ boundaries
-- on specific important threads since each thread will work on a frame at a
-- slightly different time.
-- We also compute the corrected CUJ ts boundaries. This is necessary because
-- the instrumentation logs begin/end CUJ markers *during* the first frame and
-- typically *right at the start* of the last CUJ frame. The ts boundaries in
-- `android_jank_cuj` table are based on these markers so do not actually
-- contain the whole CUJ, but instead overlap with all Choreographer#doFrame
-- slices that belong to a CUJ.
SELECT RUN_METRIC('android/jank/cujs_boundaries.sql');
-- With relevant slices and corrected boundaries we can now estimate the ts
-- boundaries of each frame within the CUJ.
-- We also match with the data from the actual timeline to check which frames
-- missed the deadline and whether this was due to the app or SF.
SELECT RUN_METRIC('android/jank/frames.sql');
-- Creates tables with slices from various relevant threads that are within
-- the CUJ boundaries. Used as data sources for further processing and
-- jank cause analysis of traces.
SELECT RUN_METRIC('android/jank/slices.sql');
-- Creates tables and functions to be used for manual investigations and
-- jank cause analysis of traces.
SELECT RUN_METRIC('android/jank/internal/query_base.sql');
SELECT RUN_METRIC('android/jank/query_functions.sql');

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
