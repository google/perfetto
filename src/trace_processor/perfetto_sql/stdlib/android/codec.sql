-- Copyright 2026 The Android Open Source Project
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

INCLUDE PERFETTO MODULE android.keyvalue_lookup;

-- Parsed codec track events.
-- Track names are expected to follow the format:
--   `codec.track.<track_event_type>.<codec_name>.<unique_no>`
-- E.g.:
--   `codec.track.state.c2.android.aac.decoder.0`
--   `codec.track.action.c2.google.av1.decoder.1`
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
  codec_events_raw AS (
    SELECT s.ts, t.name AS track_name, s.name AS atrace_payload
    FROM slice AS s
    JOIN track AS t
      ON s.track_id = t.id
    WHERE
      t.name GLOB 'codec.track.*'
      AND s.dur = 0
  ),
  codec_events_parsed AS (
    SELECT
      ts,
      track_name,
      atrace_payload,
      str_split(track_name, '.', 2) AS track_event_type,
      reverse(str_split(reverse(track_name), '.', 0)) AS unique_no
    FROM codec_events_raw
  )
SELECT
  ts,
  track_name,
  track_event_type,
  -- Index math:
  --   `codec.track.` (len 12) + `track_event_type` + `.` (len 1) = `13 + length(track_event_type)` prefix.
  --   Since `substr` is 1-indexed, the substring starts at `14 + length(track_event_type)`.
  --   Length to extract is `total_len - prefix_len - suffix_len (unique_no + dot)`.
  substr(
    track_name,
    14 + length(track_event_type),
    length(track_name) - 14 - length(track_event_type) - length(unique_no)
  ) AS codec_name,
  unique_no,
  atrace_payload,
  -- Extract key-value pairs from atrace_payload using the generic function
  _android_keyvalue_lookup_extract_key_value_arg(atrace_payload, 'event') AS event,
  _android_keyvalue_lookup_extract_key_value_arg(atrace_payload, 'metadata') AS metadata,
  _android_keyvalue_lookup_extract_key_value_arg(atrace_payload, 'info') AS info,
  CAST(_android_keyvalue_lookup_extract_key_value_arg(atrace_payload, 'pid') AS LONG) AS pid,
  CAST(_android_keyvalue_lookup_extract_key_value_arg(atrace_payload, 'uid') AS LONG) AS uid,
  _android_keyvalue_lookup_extract_key_value_arg(atrace_payload, 'render') AS render,
  CAST(_android_keyvalue_lookup_extract_key_value_arg(
    atrace_payload,
    'intervalMs'
  ) AS LONG) AS interval_ms,
  CAST(_android_keyvalue_lookup_extract_key_value_arg(atrace_payload, 'count') AS LONG) AS count,
  CAST(_android_keyvalue_lookup_extract_key_value_arg(atrace_payload, 'ctr') AS LONG) AS ctr
FROM codec_events_parsed;
