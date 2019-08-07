--
-- Copyright 2019 The Android Open Source Project
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

-- Create all the views used to generate the Android Memory metrics proto.
-- Anon RSS
SELECT RUN_METRIC('android/upid_span_view.sql',
  'table_name', 'anon_rss',
  'counter_name', 'mem.rss.anon');

-- File RSS
SELECT RUN_METRIC('android/upid_span_view.sql',
  'table_name', 'file_rss',
  'counter_name', 'mem.rss.file');

-- Swap
SELECT RUN_METRIC('android/upid_span_view.sql',
  'table_name', 'swap',
  'counter_name', 'mem.swap');

-- Anon RSS + Swap
DROP TABLE IF EXISTS anon_and_swap_join;

CREATE VIRTUAL TABLE anon_and_swap_join
USING SPAN_OUTER_JOIN(anon_rss_span PARTITIONED upid, swap_span PARTITIONED upid);

DROP VIEW IF EXISTS anon_and_swap_span;

CREATE VIEW anon_and_swap_span AS
SELECT
  ts, dur, upid,
  IFNULL(anon_rss_val, 0) + IFNULL(swap_val, 0) AS anon_and_swap_val
FROM anon_and_swap_join;

-- If we have dalvik events enabled (for ART trace points) we can construct the java heap timeline.
SELECT RUN_METRIC('android/upid_span_view.sql',
  'table_name', 'java_heap_kb',
  'counter_name', 'Heap size (KB)');

DROP VIEW IF EXISTS java_heap_span;

CREATE VIEW java_heap_span AS
SELECT ts, dur, upid, java_heap_kb_val * 1024 AS java_heap_val
FROM java_heap_kb_span;

-- Create a track for process OOM scores.
DROP VIEW IF EXISTS oom_score_span;

CREATE VIEW oom_score_span AS
SELECT
  ts,
  LEAD(ts, 1, (SELECT end_ts + 1 FROM trace_bounds))
    OVER(PARTITION BY counter_id ORDER BY ts) - ts AS dur,
  ref AS upid,
  CAST(value AS INT) AS oom_score_val
FROM counters
WHERE name = 'oom_score_adj' AND ref IS NOT NULL;

DROP TABLE IF EXISTS anon_rss_by_oom_span;

CREATE VIRTUAL TABLE anon_rss_by_oom_span
USING SPAN_JOIN(anon_rss_span PARTITIONED upid, oom_score_span PARTITIONED upid);

DROP TABLE IF EXISTS file_rss_by_oom_span;

CREATE VIRTUAL TABLE file_rss_by_oom_span
USING SPAN_JOIN(file_rss_span PARTITIONED upid, oom_score_span PARTITIONED upid);

DROP TABLE IF EXISTS swap_by_oom_span;

CREATE VIRTUAL TABLE swap_by_oom_span
USING SPAN_JOIN(swap_span PARTITIONED upid, oom_score_span PARTITIONED upid);

DROP TABLE IF EXISTS anon_and_swap_by_oom_span;

CREATE VIRTUAL TABLE anon_and_swap_by_oom_span
USING SPAN_JOIN(anon_and_swap_span PARTITIONED upid, oom_score_span PARTITIONED upid);

DROP TABLE IF EXISTS java_heap_by_oom_span;

CREATE VIRTUAL TABLE java_heap_by_oom_span
USING SPAN_JOIN(java_heap_span PARTITIONED upid, oom_score_span PARTITIONED upid);
