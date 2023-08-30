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

INCLUDE PERFETTO MODULE chrome.scroll_jank.event_latency_scroll_jank_cause;

DROP VIEW IF EXISTS __chrome_scroll_jank_v2_scroll_processing;

CREATE VIEW __chrome_scroll_jank_v2_scroll_processing
AS
SELECT COALESCE(SUM(jank.dur), 0) / 1.0e6 AS scroll_processing_ms
FROM
  chrome_scroll_event_latency_jank AS jank
LEFT JOIN
  chrome_event_latency_scroll_jank_cause AS cause
  ON
    jank.id = cause.slice_id;

DROP VIEW IF EXISTS __chrome_scroll_jank_v2_causes_and_durations;

CREATE VIEW __chrome_scroll_jank_v2_causes_and_durations
AS
SELECT
  COALESCE(SUM(jank.dur), 0) / 1.0e6 AS scroll_jank_processing_ms,
  COUNT(*) AS num_scroll_janks,
  RepeatedField(
    ChromeScrollJankV2_ScrollJankCauseAndDuration(
      'cause',
      cause.cause_of_jank,
      'sub_cause',
      cause.sub_cause_of_jank,
      'duration_ms',
      jank.dur / 1.0e6))
    AS scroll_jank_causes_and_durations
FROM
  chrome_scroll_event_latency_jank AS jank
LEFT JOIN
  chrome_event_latency_scroll_jank_cause AS cause
  ON
    jank.id = cause.slice_id
WHERE
  jank.jank AND cause.cause_of_jank != 'RendererCompositorQueueingDelay';

DROP VIEW IF EXISTS __chrome_scroll_jank_v2;

CREATE VIEW __chrome_scroll_jank_v2
AS
SELECT
  100.0 * scroll_jank_processing_ms / scroll_processing_ms
    AS scroll_jank_percentage,
  *
FROM
  (
    SELECT
      total_scroll_processing.scroll_processing_ms,
      causes_and_durations.scroll_jank_processing_ms,
      causes_and_durations.num_scroll_janks,
      causes_and_durations.scroll_jank_causes_and_durations
    FROM
      __chrome_scroll_jank_v2_scroll_processing
        AS total_scroll_processing,
      __chrome_scroll_jank_v2_causes_and_durations AS causes_and_durations
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
    scroll_jank_percentage,
    'num_scroll_janks',
    num_scroll_janks,
    'scroll_jank_causes_and_durations',
    scroll_jank_causes_and_durations)
FROM
  __chrome_scroll_jank_v2;
