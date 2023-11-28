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

--------------------------------------------------------------------------------
-- Join the relevant tracks/flows to the individual scrolls.
--------------------------------------------------------------------------------

-- Keeping only the GestureScrollUpdates join the maximum flows on the browser
-- thread.
DROP VIEW IF EXISTS scroll_with_browser_flows;
CREATE PERFETTO VIEW scroll_with_browser_flows AS
SELECT
  scroll.trace_id,
  scroll.scroll_id,
  scroll.ts,
  scroll.dur,
  scroll.track_id,
  browser_flows.ts AS browser_flow_ts,
  browser_flows.flow_step AS browser_flow_step,
  browser_flows.track_id AS browser_track_id
FROM (
  SELECT
    trace_id,
    id AS scroll_id,
    ts,
    dur,
    track_id
  FROM scroll_jank
) scroll JOIN browser_flows ON
  scroll.trace_id = browser_flows.trace_id;

--------------------------------------------------------------------------------
-- Below we determine if there was any bitmaps taken on the browser main.
--------------------------------------------------------------------------------
DROP VIEW IF EXISTS get_bitmap_calls;
CREATE PERFETTO VIEW get_bitmap_calls AS
SELECT
  id,
  ts,
  dur,
  track_id
FROM slice
WHERE
  slice.name = "ViewResourceAdapter:getBitmap"
  AND track_id = (SELECT id FROM browser_main_track_id);

DROP VIEW IF EXISTS toolbar_bitmaps;
CREATE PERFETTO VIEW toolbar_bitmaps AS
SELECT
  slice.id,
  slice.ts,
  slice.dur,
  slice.track_id,
  ancestor.id AS ancestor_id
FROM
  slice JOIN
  ancestor_slice(slice.id) AS ancestor ON
    ancestor.depth = slice.depth - 1
WHERE
  slice.name = "ToolbarLayout.draw"
  AND ancestor.name = "ViewResourceAdapter:getBitmap"
  AND slice.track_id = (SELECT id FROM browser_main_track_id);

DROP VIEW IF EXISTS get_bitmaps_and_toolbar;
CREATE PERFETTO VIEW get_bitmaps_and_toolbar AS
SELECT
  bitmap.id AS id,
  bitmap.ts AS ts,
  bitmap.dur AS dur,
  bitmap.track_id AS track_id,
  toolbar.id AS toolbar_id,
  toolbar.ts AS toolbar_ts,
  toolbar.dur AS toolbar_dur,
  toolbar.track_id AS toolbar_track_id
FROM
  get_bitmap_calls bitmap LEFT JOIN
  toolbar_bitmaps toolbar ON
    toolbar.ancestor_id = bitmap.id;

--------------------------------------------------------------------------------
-- Take bitmaps and determine if it could have been blocked by a scroll. I.E. if
-- the bitmap occurred after the start of the GestureScrollUpdate but before the
-- last flow on the browser thread (the GestureScrollUpdate can't be blocked
-- by a browser thread slice once its done on the browser thread).
--------------------------------------------------------------------------------
DROP VIEW IF EXISTS blocking_bitmap_tasks;
CREATE PERFETTO VIEW blocking_bitmap_tasks AS
SELECT
  scroll.scroll_id,
  scroll.trace_id,
  bitmap.id,
  bitmap.ts,
  bitmap.dur,
  bitmap.track_id,
  COALESCE(bitmap.track_id = scroll.browser_track_id
    AND bitmap.ts < scroll.browser_flow_ts, FALSE) AS blocked_by_bitmap,
  COALESCE(bitmap.track_id = scroll.browser_track_id
    AND bitmap.toolbar_id IS NOT NULL
    AND bitmap.ts < scroll.browser_flow_ts, FALSE) AS blocked_by_toolbar,
  COALESCE(bitmap.track_id = scroll.browser_track_id
    AND bitmap.toolbar_id IS NULL
    AND bitmap.ts < scroll.browser_flow_ts, FALSE) AS blocked_by_bitmap_no_toolbar
FROM
  scroll_with_browser_flows scroll JOIN
  get_bitmaps_and_toolbar bitmap ON
    scroll.ts + scroll.dur >= bitmap.ts
    AND bitmap.ts + bitmap.dur >= scroll.ts;


--------------------------------------------------------------------------------
-- Remove duplicate tasks blocking so that there is only a boolean per
-- scroll_id.
--------------------------------------------------------------------------------
DROP VIEW IF EXISTS scroll_jank_cause_get_bitmap;
CREATE PERFETTO VIEW scroll_jank_cause_get_bitmap AS
SELECT
  scroll_id,
  trace_id,
  SUM(blocked_by_bitmap) > 0 AS blocked_by_bitmap,
  SUM(blocked_by_toolbar) > 0 AS blocked_by_toolbar,
  SUM(blocked_by_bitmap_no_toolbar) > 0 AS blocked_by_bitmap_no_toolbar
FROM blocking_bitmap_tasks
GROUP BY 1, 2;
