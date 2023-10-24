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

-- Chrome page loads, including associated high-level metrics and properties.
--
-- @column navigation_id             ID of the navigation associated with the
--                                   page load (i.e. the cross-document
--                                   navigation in primary main frame which
--                                   created this page's main document). Also
--                                   note that navigation_id is specific to a
--                                   given Chrome browser process, and not
--                                   globally unique.
-- @column navigation_start_ts       Timestamp of the start of navigation.
-- @column fcp                       Duration between the navigation start and
--                                   the first contentful paint event
--                                   (web.dev/fcp).
-- @column fcp_ts                    Timestamp of the first contentful paint.
-- @column lcp                       Duration between the navigation start and
--                                   the largest contentful paint event
--                                   (web.dev/lcp).
-- @column lcp_ts                    Timestamp of the largest contentful paint.
-- @column url                       URL at the page load event.
-- @column browser_upid              The unique process id (upid) of the browser
--                                   process where the page load occurred.
CREATE PERFETTO TABLE chrome_page_loads AS
WITH fcp AS (
  SELECT
    ts,
    dur,
    EXTRACT_ARG(arg_set_id, 'page_load.navigation_id') AS navigation_id,
    EXTRACT_ARG(arg_set_id, 'page_load.url') AS url,
    upid AS browser_upid
  FROM process_slice
  WHERE name = 'PageLoadMetrics.NavigationToFirstContentfulPaint'
),
lcp AS (
  SELECT
    ts,
    dur,
    EXTRACT_ARG(arg_set_id, 'page_load.navigation_id')
      AS navigation_id
  FROM slice
  WHERE name = 'PageLoadMetrics.NavigationToLargestContentfulPaint'
)
SELECT
 fcp.navigation_id,
 fcp.ts AS navigation_start_ts,
 fcp.dur AS fcp,
 fcp.ts + fcp.dur AS fcp_ts,
 lcp.dur AS lcp,
 IFNULL(lcp.dur, 0) + IFNULL(lcp.ts, 0) AS lcp_ts,
 fcp.url,
 fcp.browser_upid
FROM fcp
LEFT JOIN lcp USING (navigation_id);
