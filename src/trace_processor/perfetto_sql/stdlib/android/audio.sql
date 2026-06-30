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

-- Provides unified access to Android Audio Track events from ATrace.

INCLUDE PERFETTO MODULE android.keyvalue_lookup;

-- View to get audio track usage intervals
-- Suggested minimal config:
--
-- data_sources: {
--     config: {
--         name: "linux.ftrace"
--         ftrace_config: {
--             atrace_apps: "*"
--             atrace_categories: "audio"
--         }
--     }
-- }
CREATE PERFETTO VIEW android_audio_track_state(
  -- Timestamp of audio state change.
  ts TIMESTAMP,
  -- Duration of audio state.
  dur DURATION,
  -- UID of process.
  uid LONG,
  -- PID of process.
  pid LONG,
  -- Unique identifier for the audio stream.
  track_name STRING,
  -- Content type of audio.
  content_type STRING,
  -- Usage of audio.
  usage STRING,
  -- Devices used for audio.
  devices STRING,
  -- Audio flags.
  flags STRING,
  -- Audio format.
  format STRING,
  -- Audio sample rate.
  sample_rate LONG,
  -- Audio channel mask.
  channel_mask LONG,
  -- Audio frame count.
  frame_count LONG,
  -- Thread type.
  thread_type STRING,
  -- Thread ID.
  thread_id LONG,
  -- Thread sample rate.
  thread_sample_rate LONG,
  -- Thread format.
  thread_format STRING
)
AS
WITH
  raw_events AS (
    SELECT s.ts, t.name AS track_name, s.name AS atrace_payload
    FROM slice AS s
    JOIN track AS t
      ON s.track_id = t.id
    WHERE
      t.name GLOB 'audio.track.interval.*'
  ),
  parsed_events AS (
    SELECT
      ts,
      track_name,
      _android_keyvalue_lookup_extract_key_value_arg(atrace_payload, 'event') AS event_type,
      CAST(_android_keyvalue_lookup_extract_key_value_arg(atrace_payload, 'uid') AS LONG) AS uid,
      CAST(_android_keyvalue_lookup_extract_key_value_arg(atrace_payload, 'pid') AS LONG) AS pid,
      _android_keyvalue_lookup_extract_key_value_arg(
        atrace_payload,
        'contentType'
      ) AS content_type,
      _android_keyvalue_lookup_extract_key_value_arg(atrace_payload, 'usage') AS usage,
      _android_keyvalue_lookup_extract_key_value_arg(atrace_payload, 'devices') AS devices,
      _android_keyvalue_lookup_extract_key_value_arg(atrace_payload, 'flags') AS flags,
      _android_keyvalue_lookup_extract_key_value_arg(atrace_payload, 'format') AS format,
      CAST(_android_keyvalue_lookup_extract_key_value_arg(
        atrace_payload,
        'sampleRate'
      ) AS LONG) AS sample_rate,
      CAST(_android_keyvalue_lookup_extract_key_value_arg(
        atrace_payload,
        'channelMask'
      ) AS LONG) AS channel_mask,
      CAST(_android_keyvalue_lookup_extract_key_value_arg(
        atrace_payload,
        'frameCount'
      ) AS LONG) AS frame_count,
      _android_keyvalue_lookup_extract_key_value_arg(
        atrace_payload,
        'thread.type'
      ) AS thread_type,
      CAST(_android_keyvalue_lookup_extract_key_value_arg(
        atrace_payload,
        'thread.id'
      ) AS LONG) AS thread_id,
      CAST(_android_keyvalue_lookup_extract_key_value_arg(
        atrace_payload,
        'thread.sampleRate'
      ) AS LONG) AS thread_sample_rate,
      _android_keyvalue_lookup_extract_key_value_arg(
        atrace_payload,
        'thread.format'
      ) AS thread_format
    FROM raw_events
  ),
  all_events AS (
    SELECT
      *,
      LAG(event_type) OVER (PARTITION BY track_name ORDER BY ts) AS prev_event_type
    FROM parsed_events
  ),
  classified_events AS (
    SELECT
      *,
      (event_type = 'beginInterval'
      OR (event_type = 'refreshInterval'
      AND (prev_event_type IS NULL OR prev_event_type = 'endInterval'))) AS is_start,
      (event_type = 'endInterval') AS is_end
    FROM all_events
  ),
  boundary_events AS (SELECT * FROM classified_events WHERE is_start OR is_end),
  intervals AS (
    SELECT *, LEAD(ts) OVER (PARTITION BY track_name ORDER BY ts) AS next_ts
    FROM boundary_events
  )
SELECT
  ts,
  COALESCE(next_ts, (SELECT end_ts FROM trace_bounds)) - ts AS dur,
  uid,
  pid,
  track_name,
  content_type,
  usage,
  devices,
  flags,
  format,
  sample_rate,
  channel_mask,
  frame_count,
  thread_type,
  thread_id,
  thread_sample_rate,
  thread_format
FROM intervals
WHERE
  is_start
  AND dur >= 0;
