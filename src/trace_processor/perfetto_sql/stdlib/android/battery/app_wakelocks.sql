-- Provides unified access to Android App Wakelock events.
--
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
INCLUDE PERFETTO MODULE android.battery_stats;

-- Table for app wakelocks sourced from SDK trace events.
-- This is the preferred source.
CREATE PERFETTO TABLE android_app_wakelocks_sdk (
  -- Start timestamp of the wakelock.
  ts TIMESTAMP,
  -- Duration of the wakelock. -1 if wakelock is not finished in trace.
  dur DURATION,
  -- Duration of the wakelock. If -1, clamped to trace end.
  safe_dur DURATION,
  -- Wakelock name/tag.
  name STRING,
  -- UID of the app that owns the wakelock.
  owner_uid LONG,
  -- PID of the process that owns the wakelock.
  owner_pid LONG,
  -- UID of app on behalf of which work is being done. Can be null.
  work_uid LONG,
  -- PowerManager wakelock flags.
  flags LONG
) AS
SELECT
  s.ts,
  s.dur,
  iif(s.dur = -1, trace_end() - s.ts, s.dur) AS safe_dur,
  s.name,
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
CREATE PERFETTO TABLE android_app_wakelocks_batterystats (
  -- Start timestamp of the wakelock.
  ts TIMESTAMP,
  -- Duration of the wakelock. -1 if wakelock is not finished in trace.
  dur DURATION,
  -- Duration of the wakelock. If -1, clamped to trace end.
  safe_dur DURATION,
  -- Wakelock name/tag.
  name STRING,
  -- UID of the app that owns the wakelock.
  owner_uid LONG,
  -- PID of the process that owns the wakelock.
  owner_pid LONG,
  -- UID of app on behalf of which work is being done. Can be null.
  work_uid LONG,
  -- PowerManager wakelock flags.
  flags LONG
) AS
SELECT
  ts,
  dur,
  safe_dur,
  str_value AS name,
  int_value AS owner_uid,
  NULL AS owner_pid,
  NULL AS work_uid,
  NULL AS flags
FROM android_battery_stats_event_slices
WHERE
  track_name = 'battery_stats.longwake';

-- Unified view for App Wakelocks.
-- Prioritizes SDK over BatteryStats. If SDK events exist in the trace,
-- only SDK events will be returned. Otherwise, it falls back to BatteryStats.
CREATE PERFETTO VIEW android_app_wakelocks (
  -- Start timestamp of the wakelock.
  ts TIMESTAMP,
  -- Duration of the wakelock. -1 if wakelock is not finished in trace.
  dur DURATION,
  -- Duration of the wakelock. If -1, clamped to trace end.
  safe_dur DURATION,
  -- Wakelock name/tag.
  name STRING,
  -- UID of the app that owns the wakelock.
  owner_uid LONG,
  -- PID of the process that owns the wakelock.
  owner_pid LONG,
  -- UID of app on behalf of which work is being done. Can be null.
  work_uid LONG,
  -- PowerManager wakelock flags.
  flags LONG,
  -- Which underlying data source this row comes from (sdk or battery_stats).
  data_source STRING
) AS
-- 1. Select from SDK if it exists
SELECT
  ts,
  dur,
  safe_dur,
  name,
  owner_uid,
  owner_pid,
  work_uid,
  flags,
  'sdk' AS data_source
FROM android_app_wakelocks_sdk
UNION ALL
-- 2. Fallback to BatteryStats if SDK does not exist
SELECT
  ts,
  dur,
  safe_dur,
  name,
  owner_uid,
  owner_pid,
  work_uid,
  flags,
  'battery_stats' AS data_source
FROM android_app_wakelocks_batterystats
WHERE
  NOT EXISTS(
    SELECT
      1
    FROM android_app_wakelocks_sdk
  );
