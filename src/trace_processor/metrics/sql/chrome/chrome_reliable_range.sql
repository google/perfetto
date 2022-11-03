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

-- The "reliable range" is defined as follows:
-- 1. If a thread_track has a first_packet_on_sequence flag, the thread data is reliable for the
--    entire duration of the trace.
-- 2. Otherwise, the thread data is reliable from the first thread event till the end of the trace.
-- 3. The "reliable range" is an intersection of reliable thread ranges for all threads such that:
--   a. The number of events on the thread is at or above 25p.
--   b. The event rate for the thread is at or above 75p.

DROP VIEW IF EXISTS chrome_event_stats_per_thread;

CREATE VIEW chrome_event_stats_per_thread
AS
SELECT
  COUNT(*) AS cnt, CAST(COUNT(*) AS DOUBLE) / (MAX(ts + dur) - MIN(ts)) AS rate, utid
FROM thread_track
JOIN slice
  ON thread_track.id = slice.track_id
GROUP BY utid;

DROP VIEW IF EXISTS chrome_event_cnt_cutoff;

-- Ignore the bottom 25% of threads by event count. 25% is a somewhat arbitrary number. It creates a
-- cutoff at around 10 events for a typical trace, and threads with fewer events are usually:
-- 1. Not particularly interesting for the reliable range definition.
-- 2. Create a lot of noise for other metrics, such as event rate.
CREATE VIEW chrome_event_cnt_cutoff
AS
SELECT cnt
FROM
  chrome_event_stats_per_thread
ORDER BY
  cnt
LIMIT
  1
    OFFSET(
      (SELECT COUNT(*) FROM chrome_event_stats_per_thread) / 4);

DROP VIEW IF EXISTS chrome_event_rate_cutoff;

-- Choose the top 25% event rate. 25% is a somewhat arbitrary number. The goal is to strike
-- balance between not cropping too many events and making sure that the chance of data loss in the
-- range declared "reliable" is low.
CREATE VIEW chrome_event_rate_cutoff
AS
SELECT rate
FROM
  chrome_event_stats_per_thread
ORDER BY
  rate
LIMIT
  1
    OFFSET(
      (SELECT COUNT(*) FROM chrome_event_stats_per_thread) * 3 / 4);

DROP VIEW IF EXISTS chrome_reliable_range_per_thread;

-- This view includes only threads with event count and rate above the cutoff points defined
-- above.
-- See b/239830951 for the analysis showing why we don't want to include all threads here
-- (TL;DR - it makes the "reliable range" too short for a typical trace).
CREATE VIEW chrome_reliable_range_per_thread
AS
SELECT
  MIN(ts) AS start,
  MAX(IFNULL(EXTRACT_ARG(source_arg_set_id, 'has_first_packet_on_sequence'), 0))
    AS has_first_packet_on_sequence
FROM thread_track
JOIN slice
  ON thread_track.id = slice.track_id
WHERE
  utid IN (
    SELECT utid
    FROM chrome_event_stats_per_thread
    LEFT JOIN chrome_event_cnt_cutoff
      ON 1
    LEFT JOIN chrome_event_rate_cutoff
      ON 1
    WHERE
      chrome_event_stats_per_thread.cnt >= chrome_event_cnt_cutoff.cnt
      AND chrome_event_stats_per_thread.rate >= chrome_event_rate_cutoff.rate
  )
GROUP BY utid;

DROP VIEW IF EXISTS chrome_processes_data_loss_free_period;

CREATE VIEW chrome_processes_data_loss_free_period
AS
SELECT
  -- If reliable_from is NULL, the process has data loss until the end of the trace.
  MAX(IFNULL(reliable_from, (SELECT MAX(ts + dur) FROM slice))) AS start
FROM
  experimental_missing_chrome_processes;

DROP VIEW IF EXISTS chrome_reliable_range;

CREATE VIEW chrome_reliable_range
AS
SELECT
  MAX(COALESCE(MAX(start), 0),
      COALESCE((SELECT start FROM chrome_processes_data_loss_free_period), 0)) AS start
FROM chrome_reliable_range_per_thread
WHERE has_first_packet_on_sequence = 0;
