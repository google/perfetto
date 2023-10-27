-- Copyright 2023 The Android Open Source Project
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

-- TODO(b/306300843): The recorded navigation ids are not guaranteed to be
-- unique within a trace; they are only guaranteed to be unique within a single
-- chrome instance. Chrome instance id needs to be recorded, and used here in
-- combination with navigation id to uniquely identify page load metrics.

INCLUDE PERFETTO MODULE common.slices;

CREATE VIEW internal_fcp_metrics AS
SELECT
  ts,
  dur,
  EXTRACT_ARG(arg_set_id, 'page_load.navigation_id') AS navigation_id,
  EXTRACT_ARG(arg_set_id, 'page_load.url') AS url,
  upid AS browser_upid
FROM process_slice
WHERE name = 'PageLoadMetrics.NavigationToFirstContentfulPaint';

CREATE PERFETTO FUNCTION internal_page_load_metrics(event_name STRING)
RETURNS TABLE(
  ts LONG,
  dur LONG,
  navigation_id INT
) AS
SELECT
  ts,
  dur,
  EXTRACT_ARG(arg_set_id, 'page_load.navigation_id')
    AS navigation_id
FROM slice
WHERE name = $event_name;

-- Chrome page loads, including associated high-level metrics and properties.
--
-- @column navigation_id               ID of the navigation associated with the
--                                     page load (i.e. the cross-document
--                                     navigation in primary main frame which
--                                     created this page's main document). Also
--                                     note that navigation_id is specific to a
--                                     given Chrome browser process, and not
--                                     globally unique.
-- @column navigation_start_ts         Timestamp of the start of navigation.
-- @column fcp                         Duration between the navigation start and
--                                     the first contentful paint event
--                                     (web.dev/fcp).
-- @column fcp_ts                      Timestamp of the first contentful paint.
-- @column lcp                         Duration between the navigation start and
--                                     the largest contentful paint event
--                                     (web.dev/lcp).
-- @column lcp_ts                      Timestamp of the largest contentful paint.
-- @column dom_content_loaded_event_ts Timestamp of the DomContentLoaded event.
--                                     (https://developer.mozilla.org/en-US/docs/Web/API/Document/DOMContentLoaded_event)
-- @column load_event_ts               Timestamp of the window load event.
--                                     (https://developer.mozilla.org/en-US/docs/Web/API/Window/load_event)
-- @column mark_fully_loaded_ts        Timestamp of the page self-reporting as
--                                     fully loaded through the
--                                     performance.mark('mark_fully_loaded')
--                                     API.
-- @column mark_fully_visible_ts       Timestamp of the page self-reporting as
--                                     fully visible through the
--                                     performance.mark('mark_fully_visible')
--                                     API.
-- @column mark_interactive_ts         Timestamp of the page self-reporting as
--                                     fully interactive through the
--                                     performance.mark('mark_interactive') API.
-- @column url                         URL at the page load event.
-- @column browser_upid                The unique process id (upid) of the
--                                     browser process where the page load
--                                     occurred.
CREATE PERFETTO TABLE chrome_page_loads AS
SELECT
  fcp.navigation_id,
  fcp.ts AS navigation_start_ts,
  fcp.dur AS fcp,
  fcp.ts + fcp.dur AS fcp_ts,
  lcp.dur AS lcp,
  IFNULL(lcp.dur, 0) + IFNULL(lcp.ts, 0) AS lcp_ts,
  load_fired.ts AS dom_content_loaded_event_ts,
  start_load.ts AS load_event_ts,
  timing_loaded.ts AS mark_fully_loaded_ts,
  timing_visible.ts AS mark_fully_visible_ts,
  timing_interactive.ts AS mark_interactive_ts,
  fcp.url,
  fcp.browser_upid
FROM internal_fcp_metrics fcp
LEFT JOIN
  internal_page_load_metrics('PageLoadMetrics.NavigationToLargestContentfulPaint') lcp
    USING (navigation_id)
LEFT JOIN
  internal_page_load_metrics('PageLoadMetrics.NavigationToDOMContentLoadedEventFired') load_fired
    using (navigation_id)
LEFT JOIN
  internal_page_load_metrics('PageLoadMetrics.NavigationToMainFrameOnLoad') start_load
    using (navigation_id)
LEFT JOIN
  internal_page_load_metrics('PageLoadMetrics.UserTimingMarkFullyLoaded') timing_loaded
    using (navigation_id)
LEFT JOIN
  internal_page_load_metrics('PageLoadMetrics.UserTimingMarkFullyVisible') timing_visible
    using (navigation_id)
LEFT JOIN
  internal_page_load_metrics('PageLoadMetrics.UserTimingMarkInteractive') timing_interactive
    using (navigation_id);
