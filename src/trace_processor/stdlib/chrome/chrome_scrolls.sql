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

DROP VIEW IF EXISTS chrome_scrolls;

-- Defines slices for all of the individual scrolls in a trace based on the
-- LatencyInfo-based scroll definition.
--
-- @column id            The unique identifier of the scroll.
-- @column ts            The start timestamp of the scroll.
-- @column dur           The duration of the scroll.
--
-- NOTE: this view of top level scrolls is based on the LatencyInfo definition
-- of a scroll, which differs subtly from the definition based on
-- EventLatencies.
-- TODO(b/278684408): add support for tracking scrolls across multiple Chrome/
-- WebView instances. Currently gesture_scroll_id unique within an instance, but
-- is not unique across multiple instances. Switching to an EventLatency based
-- definition of scrolls should resolve this.
CREATE TABLE chrome_scrolls AS
WITH all_scrolls AS (
  SELECT
    name,
    ts,
    dur,
    extract_arg(arg_set_id, 'chrome_latency_info.gesture_scroll_id') AS scroll_id
  FROM slice
  WHERE name GLOB 'InputLatency::GestureScroll*'
  AND extract_arg(arg_set_id, 'chrome_latency_info.gesture_scroll_id') IS NOT NULL
),
scroll_starts AS (
  SELECT
    scroll_id,
    MIN(ts) AS scroll_start_ts
  FROM all_scrolls
  WHERE name = 'InputLatency::GestureScrollBegin'
  GROUP BY scroll_id
), scroll_ends AS (
  SELECT
    scroll_id,
    MIN(ts) AS scroll_end_ts
  FROM all_scrolls
  WHERE name = 'InputLatency::GestureScrollEnd'
  GROUP BY scroll_id
)
SELECT
  sa.scroll_id AS id,
  MIN(ts) AS ts,
  CAST(MAX(ts + dur) - MIN(ts) AS INT) AS dur,
  IFNULL(ss.scroll_start_ts, -1) AS scroll_start_ts,
  IFNULL(se.scroll_end_ts, -1) AS scroll_end_ts
FROM all_scrolls sa
  LEFT JOIN scroll_starts ss ON
    sa.scroll_id = ss.scroll_id
  LEFT JOIN scroll_ends se ON
    sa.scroll_id = se.scroll_id
GROUP BY sa.scroll_id;