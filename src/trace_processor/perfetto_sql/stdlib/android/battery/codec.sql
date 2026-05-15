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

-- Provides unified access to Android Media Codec events from ATrace.
--
-- Suggested minimal config:
--
-- data_sources: {
--     config {
--         name: "linux.ftrace"
--         ftrace_config {
--             atrace_categories: "video"
--             atrace_apps: "*"
--         }
--     }
-- }

INCLUDE PERFETTO MODULE android.common.utils;

-- Table for raw codec track instant events
CREATE PERFETTO TABLE android_codec_events_raw(
  -- Timestamp of codec event.
  ts TIMESTAMP,
  -- Full track name, e.g., codec.track.state.c2.google.av1.decoder.123
  track_name STRING,
  -- Raw atrace payload, e.g., { event="allocated" pid=1234 uid=1001 }
  atrace_payload STRING
)
AS
SELECT s.ts, t.name AS track_name, s.name AS atrace_payload
FROM slice AS s
JOIN track AS t
  ON s.track_id = t.id
WHERE
  t.name GLOB 'codec.track.*'
  AND s.dur = 0;

-- Parsed codec track events
CREATE PERFETTO TABLE android_codec_events(
  -- Timestamp of codec event.
  ts TIMESTAMP,
  -- Full track name
  track_name STRING,
  -- From track name: state or action
  track_event_type STRING,
  -- From track name: e.g., c2.google.av1.decoder
  codec_name STRING,
  -- From track name: Unique instance number
  unique_no STRING,
  -- Raw atrace payload
  atrace_payload STRING,
  -- Parsed from atrace_payload: The type of codec event
  event STRING,
  -- Parsed from atrace_payload: e.g., inputFormat, outputFormat
  metadata STRING,
  -- Parsed from atrace_payload: Further details for 'configured' events
  info STRING,
  -- Parsed from atrace_payload: Process ID
  pid LONG,
  -- Parsed from atrace_payload: User ID
  uid LONG,
  -- Keep as STRING, can be CAST to BOOLEAN if needed
  -- Parsed from atrace_payload: true/false
  render STRING,
  -- Parsed from atrace_payload: Milliseconds
  interval_ms LONG,
  -- Parsed from atrace_payload: Buffer count
  count LONG,
  -- Parsed from atrace_payload: Counter (from example)
  ctr LONG
)
AS
WITH
  codeceventshelper AS (
    SELECT
      ts,
      track_name,
      atrace_payload,
      length('codec.track.') AS prefix_len,
      instr(substr(track_name, length('codec.track.') + 1), '.')
      + length('codec.track.') AS dot2_pos,
      length(track_name) - instr(reverse(track_name), '.') + 1 AS last_dot_pos
    FROM android_codec_events_raw
  )
SELECT
  ts,
  track_name,
  substr(track_name, prefix_len + 1, dot2_pos - prefix_len - 1) AS track_event_type,
  substr(track_name, dot2_pos + 1, last_dot_pos - dot2_pos - 1) AS codec_name,
  substr(track_name, last_dot_pos + 1) AS unique_no,
  atrace_payload,
  -- Extract key-value pairs from atrace_payload using the generic function
  android_common_extract_key_value_arg(atrace_payload, 'event') AS event,
  android_common_extract_key_value_arg(atrace_payload, 'metadata') AS metadata,
  android_common_extract_key_value_arg(atrace_payload, 'info') AS info,
  CAST(android_common_extract_key_value_arg(atrace_payload, 'pid') AS LONG) AS pid,
  CAST(android_common_extract_key_value_arg(atrace_payload, 'uid') AS LONG) AS uid,
  android_common_extract_key_value_arg(atrace_payload, 'render') AS render,
  CAST(android_common_extract_key_value_arg(atrace_payload, 'intervalMs') AS LONG) AS interval_ms,
  CAST(android_common_extract_key_value_arg(atrace_payload, 'count') AS LONG) AS count,
  CAST(android_common_extract_key_value_arg(atrace_payload, 'ctr') AS LONG) AS ctr
FROM codeceventshelper;
