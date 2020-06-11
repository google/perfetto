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

-- WebView is embedded in the hosting app's main process, which means it shares some threads
-- with the host app's work. In the future, we will approximate WebView-related power usage
-- by selecting user slices that belong to WebView and estimating their power use
-- through the CPU time they consume at different core frequencies.
-- The initial version of this metric calculates the total time (in nanoseconds)
-- WebView slices spend in different frequency ranges.

SELECT RUN_METRIC('android/android_cpu.sql');

DROP VIEW IF EXISTS webview_slice;

CREATE VIEW webview_slice AS
  SELECT *
  FROM
    -- TODO(b/156788923): add better conditions.
    slice WHERE category = 'android_webview';

DROP VIEW IF EXISTS top_level_webview_slice;

-- Since we filter out some slices in webview_slice above, we cannot use the "depth" column
-- to select only the top-level webview slices. Instead, we have to join webview_slice with itself,
-- selecting only those events that do not have any children in webview_slice.
CREATE VIEW top_level_webview_slice AS
  SELECT *
  FROM
    webview_slice s1 WHERE
  (SELECT COUNT(1)
  FROM (
    SELECT *
    FROM
      webview_slice s2
    WHERE s1.track_id = s2.track_id
    AND s2.ts < s1.ts
    AND s2.ts + s2.dur > s1.ts + s1.dur
    LIMIT 1)
   ) == 0;

DROP VIEW IF EXISTS slices_threads;

CREATE VIEW slices_threads AS
  SELECT
    top_level_webview_slice.ts,
    top_level_webview_slice.dur,
    thread_track.utid
FROM top_level_webview_slice INNER JOIN thread_track
  ON top_level_webview_slice.track_id = thread_track.id;

DROP TABLE IF EXISTS slices_freq;

CREATE VIRTUAL TABLE slices_freq
  USING SPAN_JOIN (cpu_freq_sched_per_thread PARTITIONED utid,
                   slices_threads PARTITIONED utid);

-- Get frequencies by utid and cpu.
SELECT RUN_METRIC('android/android_cpu_raw_metrics_per_core.sql',
  'input_table', 'slices_freq',
  'output_table', 'webview_raw_metrics_per_core');

-- TODO(b/155980166): use another query to estimate power consumption based on frequencies.
