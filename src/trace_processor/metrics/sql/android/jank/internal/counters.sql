--
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

DROP TABLE IF EXISTS android_jank_cuj_counter;
CREATE TABLE android_jank_cuj_counter AS
WITH cuj_counter_track AS (
  SELECT
    upid,
    track.id AS track_id,
    -- extract the CUJ name inside <>
    STR_SPLIT(STR_SPLIT(track.name, '>#', 0), '<', 1) AS cuj_name,
    -- take the name of the counter after #
    STR_SPLIT(track.name, '#', 1) AS counter_name
  FROM process_counter_track track
  JOIN android_jank_cuj USING (upid)
  WHERE track.name GLOB 'J<*>#*'
)
SELECT
  ts,
  upid,
  cuj_name,
  counter_name,
  CAST(value AS INTEGER) AS value
FROM counter
JOIN cuj_counter_track ON counter.track_id = cuj_counter_track.track_id;

SELECT CREATE_FUNCTION(
  'ANDROID_JANK_CUJ_COUNTER_VALUE(cuj_name STRING, counter_name STRING, ts_min INT, ts_max INT)',
  'INT',
  '
  SELECT value
  FROM android_jank_cuj_counter
  WHERE
    cuj_name = $cuj_name
    AND counter_name = $counter_name
    AND ts >= $ts_min
    AND ($ts_max IS NULL OR ts <= $ts_max)
  ORDER BY ts ASC LIMIT 1
  '
);

DROP TABLE IF EXISTS android_jank_cuj_counter_metrics;
CREATE TABLE android_jank_cuj_counter_metrics AS
-- Order CUJs to get the ts of the next CUJ with the same name.
-- This is to avoid selecting counters logged for the next CUJ in case multiple
-- CUJs happened in a short succession.
WITH cujs_ordered AS (
  SELECT
    cuj_id,
    cuj_name,
    upid,
    state,
    ts_end,
    CASE
      WHEN process_name GLOB 'com.android.*' THEN ts_end
      WHEN process_name = 'com.google.android.apps.nexuslauncher' THEN ts_end
      -- Some processes publish counters just before logging the CUJ end
      ELSE MAX(ts, ts_end - 4000000)
    END AS ts_earliest_allowed_counter,
    LEAD(ts_end) OVER (PARTITION BY cuj_name ORDER BY ts_end ASC) AS ts_end_next_cuj
  FROM android_jank_cuj
)
SELECT
  cuj_id,
  cuj_name,
  upid,
  state,
  ANDROID_JANK_CUJ_COUNTER_VALUE(cuj_name, 'totalFrames', ts_earliest_allowed_counter, ts_end_next_cuj) AS total_frames,
  ANDROID_JANK_CUJ_COUNTER_VALUE(cuj_name, 'missedFrames', ts_earliest_allowed_counter, ts_end_next_cuj) AS missed_frames,
  ANDROID_JANK_CUJ_COUNTER_VALUE(cuj_name, 'missedAppFrames', ts_earliest_allowed_counter, ts_end_next_cuj) AS missed_app_frames,
  ANDROID_JANK_CUJ_COUNTER_VALUE(cuj_name, 'missedSfFrames', ts_earliest_allowed_counter, ts_end_next_cuj) AS missed_sf_frames,
  ANDROID_JANK_CUJ_COUNTER_VALUE(cuj_name, 'maxSuccessiveMissedFrames', ts_earliest_allowed_counter, ts_end_next_cuj) AS missed_frames_max_successive,
  -- convert ms to nanos to align with the unit for `dur` in the other tables
  ANDROID_JANK_CUJ_COUNTER_VALUE(cuj_name, 'maxFrameTimeMillis', ts_earliest_allowed_counter, ts_end_next_cuj) * 1000000 AS frame_dur_max
FROM cujs_ordered cuj;
