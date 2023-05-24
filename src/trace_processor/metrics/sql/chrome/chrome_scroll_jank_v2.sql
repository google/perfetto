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
--
-- A collection of metrics related to janky scrolling. Please refer to the
-- corresponding `chrome/scroll_jank_v2.proto` for more details.

SELECT RUN_METRIC('chrome/event_latency_scroll_jank_cause.sql');

DROP VIEW IF EXISTS chrome_scroll_jank_v2;

CREATE VIEW chrome_scroll_jank_v2
AS
SELECT
  100.0 * scroll_jank_processing_ms / scroll_processing_ms
    AS scroll_jank_percentage,
  *
FROM
  (
    SELECT
      COALESCE(SUM(jank.dur), 0) / 1.0e6 AS scroll_processing_ms,
      COALESCE(
        SUM(
          CASE
            WHEN
              jank.jank
              AND cause.cause_of_jank != 'RendererCompositorQueueingDelay'
              THEN jank.dur
            ELSE 0
            END),
        0)
        / 1.0e6 AS scroll_jank_processing_ms
    FROM
      scroll_event_latency_jank AS jank
    LEFT JOIN
      event_latency_scroll_jank_cause AS cause
      ON
        jank.id = cause.slice_id
  );

DROP VIEW IF EXISTS chrome_scroll_jank_v2_output;

CREATE VIEW chrome_scroll_jank_v2_output
AS
SELECT
  ChromeScrollJankV2(
    'scroll_processing_ms',
    scroll_processing_ms,
    'scroll_jank_processing_ms',
    scroll_jank_processing_ms,
    'scroll_jank_percentage',
    scroll_jank_percentage)
FROM
  chrome_scroll_jank_v2;
