--
-- Copyright 2020 The Android Open Source Project
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

-- Needed for the scroll_jank table to tell which updates were janky.
SELECT RUN_METRIC('chrome/scroll_jank.sql');

--------------------------------------------------------------------------------
-- Get all the track ids relevant to the critical path.
--------------------------------------------------------------------------------

-- Grab the track of the browser. sendTouchEvent is a Java category event which
-- only occurs on the browser. This saves us the trouble of dealing with all the
-- different possible names of the browser (when including system tracing).
DROP VIEW IF EXISTS browser_main_track_id;
CREATE PERFETTO VIEW browser_main_track_id AS
SELECT
  track_id AS id
FROM slice
WHERE
  name = "sendTouchEvent"
LIMIT 1;

DROP VIEW IF EXISTS viz_compositor_track_id;
CREATE PERFETTO VIEW viz_compositor_track_id AS
SELECT
  id
FROM thread_track
WHERE
  utid = (
    SELECT
      utid
    FROM thread
    WHERE
      name = "VizCompositorThread"
  )
LIMIT 1;

-- Grab the track of the GPU. gpu/command_buffer is a toplevel category event
-- which only occurs on the gpu main. This saves us the trouble of dealing with
-- all the different possible names of the GPU process (when including system
-- tracing).
DROP VIEW IF EXISTS gpu_main_track_id;
CREATE PERFETTO VIEW gpu_main_track_id AS
SELECT
  track_id AS id
FROM slice
WHERE
  EXTRACT_ARG(arg_set_id, "task.posted_from.file_name") GLOB
  "*gpu/command_buffer/service/scheduler.cc"
LIMIT 1;

-- TODO(nuskos): Determine a good way to get all the renderer track_ids (each
--               scroll will have a single renderer main and a single renderer
--               compositor however different scroll updates could have
--               DIFFERENT renderers so bit tricky). Ignore this complexity for
--               now until we have a single task we want to blame jank on.

--------------------------------------------------------------------------------
-- Grab the last LatencyInfo.Flow for each trace_id on the browser main.
--------------------------------------------------------------------------------
DROP VIEW IF EXISTS browser_flows;
CREATE PERFETTO VIEW browser_flows AS
SELECT
  EXTRACT_ARG(arg_set_id, "chrome_latency_info.trace_id") AS trace_id,
  EXTRACT_ARG(arg_set_id, "chrome_latency_info.step") AS flow_step,
  track_id,
  max(ts) AS ts
FROM slice
WHERE
  track_id = (
    SELECT id FROM browser_main_track_id
  )
  AND name = "LatencyInfo.Flow"
GROUP BY trace_id;

-- Grab the last LatencyInfo.Flow for each trace_id on the VizCompositor.
DROP VIEW IF EXISTS viz_flows;
CREATE PERFETTO VIEW viz_flows AS
SELECT
  EXTRACT_ARG(arg_set_id, "chrome_latency_info.trace_id") AS trace_id,
  EXTRACT_ARG(arg_set_id, "chrome_latency_info.step") AS flow_step,
  track_id,
  max(ts) AS ts
FROM slice
WHERE
  track_id = (
    SELECT id FROM viz_compositor_track_id
  )
  AND name = "LatencyInfo.Flow"
GROUP BY trace_id;

-- Grab the last LatencyInfo.Flow for each trace_id on the GPU main.
DROP VIEW IF EXISTS gpu_flows;
CREATE PERFETTO VIEW gpu_flows AS
SELECT
  EXTRACT_ARG(arg_set_id, "chrome_latency_info.trace_id") AS trace_id,
  EXTRACT_ARG(arg_set_id, "chrome_latency_info.step") AS flow_step,
  track_id,
  max(ts) AS ts
FROM slice
WHERE
  track_id = (
    SELECT id FROM gpu_main_track_id
  )
  AND name = "LatencyInfo.Flow"
GROUP BY trace_id;

--------------------------------------------------------------------------------
-- Finally join the relevant tracks/flows to the individual scrolls.
--------------------------------------------------------------------------------

-- Keeping only the GestureScrollUpdates join the maximum flows with their
-- associated scrolls. We only keep non-coalesced scrolls.
DROP VIEW IF EXISTS scroll_with_browser_gpu_and_viz_flows;
CREATE PERFETTO VIEW scroll_with_browser_gpu_and_viz_flows AS
SELECT
  scroll.trace_id,
  scroll.scroll_id,
  scroll.ts,
  scroll.dur,
  scroll.track_id,
  browser_flows.ts AS browser_flow_ts,
  browser_flows.flow_step AS browser_flow_step,
  browser_flows.track_id AS browser_track_id,
  viz_flows.ts AS viz_flow_ts,
  viz_flows.flow_step AS viz_flow_step,
  viz_flows.track_id AS viz_track_id,
  gpu_flows.ts AS gpu_flow_ts,
  gpu_flows.flow_step AS gpu_flow_step,
  gpu_flows.track_id AS gpu_track_id
FROM (
  SELECT
    trace_id,
    id AS scroll_id,
    ts,
    dur,
    track_id
  FROM scroll_jank
) scroll JOIN browser_flows ON
  scroll.trace_id = browser_flows.trace_id
JOIN viz_flows ON viz_flows.trace_id = scroll.trace_id
JOIN gpu_flows ON gpu_flows.trace_id = scroll.trace_id;

--------------------------------------------------------------------------------
-- Below we determine individual causes of blocking tasks.
--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
-- Determine if a CopyOutputRequest blocked any important threads.
--------------------------------------------------------------------------------

-- These are the events that block the Browser Main or the VizCompositor thread.
DROP VIEW IF EXISTS blocking_browser_gpu_and_viz_copies;
CREATE PERFETTO VIEW blocking_browser_gpu_and_viz_copies AS
SELECT
  id,
  ts,
  dur,
  track_id
FROM slice
WHERE
  (
    (
      name = "viz.mojom.CopyOutputResultSender"
      OR name = "GLRenderer::CopyDrawnRenderPass"
    )
    AND track_id = (SELECT id FROM browser_main_track_id)
  ) OR (
    EXTRACT_ARG(arg_set_id, "task.posted_from.file_name") GLOB
    "*components/viz/common/frame_sinks/copy_output_request.cc"
    AND track_id = (SELECT id FROM viz_compositor_track_id)
  ) OR (
    name = "SkiaOutputSurfaceImplOnGpu::CopyOutput"
    AND track_id = (SELECT id FROM gpu_main_track_id)
  );

-- Determine based on the LatencyInfo.Flow timestamp and the copy task overlap
-- if this scroll might have been delayed because of the copy.
DROP VIEW IF EXISTS blocking_copy_tasks;
CREATE PERFETTO VIEW blocking_copy_tasks AS
SELECT
  scroll.scroll_id,
  scroll.trace_id,
  copy.id,
  copy.ts,
  copy.dur,
  copy.track_id,
  CASE WHEN copy.track_id = scroll.browser_track_id THEN
    COALESCE(copy.ts < scroll.browser_flow_ts, FALSE)
    WHEN copy.track_id = scroll.viz_track_id THEN
      COALESCE(copy.ts < scroll.viz_flow_ts, FALSE)
    WHEN copy.track_id = scroll.gpu_track_id THEN
      COALESCE(copy.ts < scroll.gpu_flow_ts, FALSE)
    ELSE
      FALSE
  END AS blocked_by_copy
FROM
  scroll_with_browser_gpu_and_viz_flows scroll JOIN
  blocking_browser_gpu_and_viz_copies copy ON
    scroll.ts + scroll.dur >= copy.ts
    AND copy.ts + copy.dur >= scroll.ts;

-- Group by scroll so we can equally join one reply to the ScrollJankAndCauses
-- view.
DROP VIEW IF EXISTS screenshot_overlapping_scrolls;
CREATE PERFETTO VIEW screenshot_overlapping_scrolls AS
SELECT
  scroll_id, trace_id, SUM(blocked_by_copy) > 0 AS blocked_by_copy_request
FROM blocking_copy_tasks
GROUP BY 1, 2;

--------------------------------------------------------------------------------
-- Check for blocking language_detection on the browser thread
--------------------------------------------------------------------------------
DROP VIEW IF EXISTS blocking_browser_language_detection;
CREATE PERFETTO VIEW blocking_browser_language_detection AS
SELECT
  id,
  ts,
  dur,
  track_id
FROM slice
WHERE
  (
    name = "language_detection.mojom.LanguageDetectionService"
    AND track_id = (SELECT id FROM browser_main_track_id)
  );

DROP VIEW IF EXISTS blocking_language_detection_tasks;
CREATE PERFETTO VIEW blocking_language_detection_tasks AS
SELECT
  scroll.scroll_id,
  scroll.trace_id,
  lang.id,
  lang.ts,
  lang.dur,
  lang.track_id,
  CASE WHEN lang.track_id = scroll.browser_track_id THEN
    COALESCE(lang.ts < scroll.browser_flow_ts, FALSE)
  END AS blocked_by_language_detection
FROM
  scroll_with_browser_gpu_and_viz_flows scroll JOIN
  blocking_browser_language_detection lang ON
    scroll.ts + scroll.dur >= lang.ts
    AND lang.ts + lang.dur >= scroll.ts;

DROP VIEW IF EXISTS language_detection_overlapping_scrolls;
CREATE PERFETTO VIEW language_detection_overlapping_scrolls AS
SELECT
  scroll_id, trace_id,
  SUM(blocked_by_language_detection) > 0 AS blocked_by_language_detection
FROM blocking_language_detection_tasks
GROUP BY 1, 2;

--------------------------------------------------------------------------------
-- Finally join the causes together for easy grouping.
--------------------------------------------------------------------------------
DROP VIEW IF EXISTS scroll_jank_cause_blocking_task;
CREATE PERFETTO VIEW scroll_jank_cause_blocking_task AS
SELECT
  lang.scroll_id,
  lang.blocked_by_language_detection,
  copy.blocked_by_copy_request
FROM
  language_detection_overlapping_scrolls lang JOIN
  screenshot_overlapping_scrolls copy ON copy.scroll_id = lang.scroll_id;
