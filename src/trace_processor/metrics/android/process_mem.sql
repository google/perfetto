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

SELECT RUN_METRIC('android/span_view_stats.sql', 'table_name', 'anon_rss');

-- File RSS
SELECT RUN_METRIC('android/upid_span_view.sql',
  'table_name', 'file_rss',
  'counter_name', 'mem.rss.file');

SELECT RUN_METRIC('android/span_view_stats.sql', 'table_name', 'file_rss');

-- Swap
SELECT RUN_METRIC('android/upid_span_view.sql',
  'table_name', 'swap',
  'counter_name', 'mem.swap');

SELECT RUN_METRIC('android/span_view_stats.sql', 'table_name', 'swap');

-- Anon RSS + Swap
DROP TABLE IF EXISTS anon_and_swap_join;

CREATE VIRTUAL TABLE anon_and_swap_join
USING SPAN_JOIN(anon_rss_span PARTITIONED upid, swap_span PARTITIONED upid);

DROP VIEW IF EXISTS anon_and_swap_span;

CREATE VIEW anon_and_swap_span AS
SELECT ts, dur, upid, anon_rss_val + swap_val AS anon_and_swap_val
FROM anon_and_swap_join;

SELECT RUN_METRIC('android/span_view_stats.sql', 'table_name', 'anon_and_swap');
