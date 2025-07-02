--
-- Copyright 2025 The Android Open Source Project
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

INCLUDE PERFETTO MODULE android.cujs.sysui_cujs;

-- Extract CUJ and missed callback slice names.
CREATE PERFETTO VIEW _marker_missed_callback AS
SELECT
  marker_track.name AS cuj_slice_name,
  marker.ts,
  marker.name AS marker_name
FROM slice AS marker
JOIN track AS marker_track
  ON marker_track.id = marker.track_id
WHERE
  marker.name GLOB '*FT#Missed*';

-- Extract count of a missed callback for a specific CUJ.
CREATE PERFETTO FUNCTION _android_cuj_missed_vsyncs_for_callback(
    -- name of the cuj slice.
    cuj_slice_name STRING,
    -- min ts.
    ts_min TIMESTAMP,
    -- max ts.
    ts_max TIMESTAMP,
    -- missed callback.
    callback_missed STRING
)
RETURNS LONG AS
SELECT
  coalesce(sum(marker_name GLOB $callback_missed), 0)
FROM _marker_missed_callback
WHERE
  cuj_slice_name = $cuj_slice_name
  AND ts >= $ts_min
  AND (
    $ts_max IS NULL OR ts <= $ts_max
  )
ORDER BY
  ts ASC
LIMIT 1;

-- Extract all counters for Jank CUJ tracks.
CREATE PERFETTO VIEW _android_jank_cuj_counter AS
WITH
  cuj_counter_track AS (
    SELECT DISTINCT
      upid,
      track.id AS track_id,
      -- extract the CUJ name inside <>
      str_split(str_split(track.name, '>#', 0), '<', 1) AS cuj_name,
      -- take the name of the counter after #
      str_split(track.name, '#', 1) AS counter_name
    FROM process_counter_track AS track
    JOIN android_sysui_jank_cujs
      USING (upid)
    WHERE
      track.name GLOB 'J<*>#*'
  )
SELECT
  ts,
  upid,
  cuj_name,
  counter_name,
  CAST(value AS INTEGER) AS value
FROM counter
JOIN cuj_counter_track
  ON counter.track_id = cuj_counter_track.track_id;

-- Returns the counter value for the given CUJ name and counter name.
CREATE PERFETTO FUNCTION _android_jank_cuj_counter_value(
    cuj_name STRING,
    counter_name STRING,
    ts_min TIMESTAMP,
    ts_max TIMESTAMP
)
RETURNS LONG AS
SELECT
  value
FROM _android_jank_cuj_counter
WHERE
  cuj_name = $cuj_name
  AND counter_name = $counter_name
  AND ts >= $ts_min
  AND (
    $ts_max IS NULL OR ts <= $ts_max
  )
ORDER BY
  ts ASC
LIMIT 1;
