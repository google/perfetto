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

-- Provides unified access to Android Audio Track events from ATrace.
--
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

INCLUDE PERFETTO MODULE android.common.utils;

-- Table for raw audio track interval events
CREATE PERFETTO TABLE android_audio_track_interval_events (
  -- Timestamp of audio event.
  ts TIMESTAMP,
  -- Unique identifier for the audio stream.
  track_name STRING,
  -- Raw atrace payload.
  atrace_payload STRING
) AS
SELECT
  s.ts,
  -- This is the unique identifier for the audio stream
  t.name AS track_name,
  s.name AS atrace_payload
FROM slice AS s
JOIN track AS t
  ON s.track_id = t.id
WHERE
  t.name GLOB 'audio.track.interval.*';

-- Parsed audio track events
CREATE PERFETTO TABLE android_audio_track_parsed_events (
  -- Timestamp of audio event.
  ts TIMESTAMP,
  -- Unique identifier for the audio stream.
  track_name STRING,
  -- Event type (beginInterval, endInterval, refreshInterval).
  event_type STRING,
  -- UID of process.
  uid LONG,
  -- PID of process.
  pid LONG,
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
) AS
SELECT
  ts,
  track_name,
  -- Extract common fields
  android_common_extract_key_value_arg(atrace_payload, 'event') AS event_type,
  CAST(android_common_extract_key_value_arg(atrace_payload, 'uid') AS LONG) AS uid,
  CAST(android_common_extract_key_value_arg(atrace_payload, 'pid') AS LONG) AS pid,
  android_common_extract_key_value_arg(atrace_payload, 'contentType') AS content_type,
  android_common_extract_key_value_arg(atrace_payload, 'usage') AS usage,
  android_common_extract_key_value_arg(atrace_payload, 'devices') AS devices,
  android_common_extract_key_value_arg(atrace_payload, 'flags') AS flags,
  android_common_extract_key_value_arg(atrace_payload, 'format') AS format,
  CAST(android_common_extract_key_value_arg(atrace_payload, 'sampleRate') AS LONG) AS sample_rate,
  CAST(android_common_extract_key_value_arg(atrace_payload, 'channelMask') AS LONG) AS channel_mask,
  CAST(android_common_extract_key_value_arg(atrace_payload, 'frameCount') AS LONG) AS frame_count,
  -- Thread specific fields
  android_common_extract_key_value_arg(atrace_payload, 'thread.type') AS thread_type,
  CAST(android_common_extract_key_value_arg(atrace_payload, 'thread.id') AS LONG) AS thread_id,
  CAST(android_common_extract_key_value_arg(atrace_payload, 'thread.sampleRate') AS LONG) AS thread_sample_rate,
  android_common_extract_key_value_arg(atrace_payload, 'thread.format') AS thread_format
-- Add other fields from the example payload as needed
FROM android_audio_track_interval_events;

-- View to get audio track usage intervals
CREATE PERFETTO VIEW android_audio_track_state (
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
) AS
WITH
  audioevents AS (
    SELECT
      *,
      lag(event_type) OVER (PARTITION BY track_name ORDER BY ts) AS prev_event_type
    FROM android_audio_track_parsed_events
  ),
  audiostarts AS (
    SELECT
      *
    FROM audioevents
    WHERE
      event_type = 'beginInterval'
      OR (
        event_type = 'refreshInterval'
        AND (
          prev_event_type IS NULL OR prev_event_type = 'endInterval'
        )
      )
  ),
  audioends AS (
    SELECT
      *
    FROM android_audio_track_parsed_events
    WHERE
      event_type = 'endInterval'
  )
SELECT
  a_start.ts AS ts,
  min(coalesce(a_end.ts, (
    SELECT
      end_ts
    FROM trace_bounds
  ))) - a_start.ts AS dur,
  a_start.uid,
  a_start.pid,
  a_start.track_name,
  a_start.content_type,
  a_start.usage,
  a_start.devices,
  a_start.flags,
  a_start.format,
  a_start.sample_rate,
  a_start.channel_mask,
  a_start.frame_count,
  a_start.thread_type,
  a_start.thread_id,
  a_start.thread_sample_rate,
  a_start.thread_format
FROM audiostarts AS a_start
LEFT JOIN audioends AS a_end
  ON a_start.track_name = a_end.track_name AND a_start.ts < a_end.ts
GROUP BY
  a_start.track_name,
  a_start.ts
HAVING
  dur >= 0;
