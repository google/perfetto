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
-- with the host app's work. We approximate WebView-related power usage
-- by selecting user slices that belong to WebView and estimating their power use
-- through the CPU time they consume at different core frequencies.
-- This file populates a summary table that can be used to produce metrics in different formats.

SELECT RUN_METRIC('android/android_proxy_power.sql');

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

DROP VIEW IF EXISTS webview_threads;

-- Match WebView slices to threads and processes.
-- Process information will be used in the future to determine the contribution of all
-- WebView process types (browser, renderer) to a specific app's power usage.
CREATE VIEW webview_threads AS
  SELECT
    top_level_webview_slice.ts,
    top_level_webview_slice.dur,
    thread_track.utid,
    process.name AS app_name
FROM top_level_webview_slice
  INNER JOIN thread_track
  ON top_level_webview_slice.track_id = thread_track.id
  INNER JOIN process
  ON thread.upid = process.upid
  INNER JOIN thread
  ON thread_track.utid = thread.utid;

DROP TABLE IF EXISTS webview_power;

-- Assign power usage to WebView slices.
CREATE VIRTUAL TABLE webview_power
USING SPAN_JOIN(power_per_thread PARTITIONED utid,
               webview_threads PARTITIONED utid);

DROP VIEW IF EXISTS webview_power_summary;

-- Calculate the power usage of all WebView slices for each app in milliampere-seconds.
CREATE VIEW webview_power_summary AS
SELECT
  app_name,
  SUM(dur * COALESCE(power_ma, 0) / 1e9) AS webview_power_mas
  FROM webview_power
GROUP BY app_name;
