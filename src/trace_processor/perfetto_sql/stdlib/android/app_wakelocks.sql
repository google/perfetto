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

-- Provides unified access to Android App Wakelock events.
INCLUDE PERFETTO MODULE android.battery_stats;

-- Table for app wakelocks sourced from SDK trace events.
-- This is the preferred source.
-- Suggested minimal config:
--
-- data_sources: {
--   config: {
--     name: "android.app_wakelocks"
--   }
-- }
CREATE PERFETTO TABLE android_app_wakelocks_sdk(
  -- Start timestamp of the wakelock.
  ts TIMESTAMP,
  -- Duration of the wakelock. If unfinished in trace, clamped to trace end.
  dur DURATION,
  -- Wakelock name/tag.
  name STRING,
  -- Overall UID (prefers work_uid, falls back to owner_uid).
  uid LONG,
  -- UID of the app that owns the wakelock.
  owner_uid LONG,
  -- PID of the process that owns the wakelock.
  owner_pid LONG,
  -- UID of app on behalf of which work is being done.
  work_uid LONG,
  -- PowerManager wakelock flags.
  flags LONG
)
AS
SELECT
  s.ts,
  iif(s.dur = -1, trace_end() - s.ts, s.dur) AS dur,
  s.name,
  coalesce(
    extract_arg(s.arg_set_id, 'work_uid'),
    extract_arg(s.arg_set_id, 'owner_uid')
  ) AS uid,
  extract_arg(s.arg_set_id, 'owner_uid') AS owner_uid,
  extract_arg(s.arg_set_id, 'owner_pid') AS owner_pid,
  extract_arg(s.arg_set_id, 'work_uid') AS work_uid,
  extract_arg(s.arg_set_id, 'flags') AS flags
FROM slice AS s
JOIN track AS t
  ON s.track_id = t.id
WHERE
  t.name = 'app_wakelock_events';

-- Table for app wakelocks sourced from BatteryStats.
-- This is a fallback for traces that do not contain the SDK events.
-- Suggested minimal config:
--
-- data_sources: {
--   config: {
--     name: "linux.ftrace"
--     ftrace_config: {
--       atrace_apps: "*"
--       atrace_categories: "power"
--     }
--   }
-- }
CREATE PERFETTO TABLE android_app_wakelocks_batterystats(
  -- Start timestamp of the wakelock.
  ts TIMESTAMP,
  -- Duration of the wakelock. If unfinished in trace, clamped to trace end.
  dur DURATION,
  -- Wakelock name/tag.
  name STRING,
  -- UID of the app that owns the wakelock (resolved to work_uid or owner_uid at source).
  uid LONG
)
AS
SELECT ts, safe_dur AS dur, str_value AS name, int_value AS uid
FROM android_battery_stats_event_slices
WHERE
  track_name = 'battery_stats.longwake';

-- Unified view for App Wakelocks.
-- Prioritizes SDK over BatteryStats. If SDK events exist in the trace,
-- only SDK events will be returned. Otherwise, it falls back to BatteryStats.
CREATE PERFETTO VIEW android_app_wakelocks(
  -- Start timestamp of the wakelock.
  ts TIMESTAMP,
  -- Duration of the wakelock. If unfinished in trace, clamped to trace end.
  dur DURATION,
  -- Wakelock name/tag.
  name STRING,
  -- Overall UID.
  uid LONG
)
AS
-- 1. Select from SDK if it exists
SELECT ts, dur, name, uid FROM android_app_wakelocks_sdk
UNION ALL
-- 2. Fallback to BatteryStats if SDK does not exist
SELECT ts, dur, name, uid
FROM android_app_wakelocks_batterystats
WHERE
  NOT EXISTS (SELECT 1 FROM android_app_wakelocks_sdk);
