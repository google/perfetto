--
-- Copyright 2019 The Android Open Source Project
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
--
INCLUDE PERFETTO MODULE android.battery;
INCLUDE PERFETTO MODULE android.battery_stats;

DROP VIEW IF EXISTS battery_view;
CREATE VIEW battery_view AS
SELECT * FROM android_battery_charge;

DROP TABLE IF EXISTS android_batt_wakelocks_merged;
CREATE PERFETTO TABLE android_batt_wakelocks_merged AS
SELECT
  MIN(ts) AS ts,
  MAX(ts_end) AS ts_end
FROM (
    SELECT
      *,
      SUM(new_group) OVER (ORDER BY ts) AS group_id
    FROM (
        SELECT
          ts,
          ts + dur AS ts_end,
          -- There is a new group if there was a gap before this wakelock.
          -- i.e. the max end timestamp of all preceding wakelocks is before
          -- the start timestamp of this one.
          -- The null check is for the first row which is always a new group.
          IFNULL(
            MAX(ts + dur) OVER (
              ORDER BY ts
              ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
            ) < ts,
            TRUE
          ) AS new_group
        FROM slice
        WHERE slice.name GLOB 'WakeLock *' AND dur != -1
    )
)
GROUP BY group_id;

DROP VIEW IF EXISTS suspend_slice_from_minimal;
CREATE VIEW suspend_slice_from_minimal AS
SELECT ts, dur
FROM track t JOIN slice s ON s.track_id = t.id
WHERE t.name = 'Suspend/Resume Minimal';

DROP TABLE IF EXISTS suspend_slice_;
CREATE PERFETTO TABLE suspend_slice_ AS
SELECT ts, dur FROM suspend_slice_from_minimal
UNION ALL
SELECT
  ts,
  dur
FROM
  slice
JOIN
  track
  ON slice.track_id = track.id
WHERE
  track.name = 'Suspend/Resume Latency'
  AND (slice.name = 'syscore_resume(0)' OR slice.name = 'timekeeping_freeze(0)')
  AND dur != -1
  AND NOT EXISTS(SELECT * FROM suspend_slice_from_minimal);

DROP VIEW suspend_slice_from_minimal;

SELECT RUN_METRIC('android/counter_span_view_merged.sql',
  'table_name', 'screen_state',
  'counter_name', 'ScreenState');

SELECT RUN_METRIC('android/counter_span_view_merged.sql',
  'table_name', 'doze_light_state',
  'counter_name', 'DozeLightState');

SELECT RUN_METRIC('android/counter_span_view_merged.sql',
  'table_name', 'doze_deep_state',
  'counter_name', 'DozeDeepState');

SELECT RUN_METRIC('android/counter_span_view_merged.sql',
  'table_name', 'battery_status',
  'counter_name', 'BatteryStatus');

SELECT RUN_METRIC('android/counter_span_view_merged.sql',
  'table_name', 'plug_type',
  'counter_name', 'PlugType');

DROP TABLE IF EXISTS screen_state_span_with_suspend;
CREATE VIRTUAL TABLE screen_state_span_with_suspend
USING span_join(screen_state_span, suspend_slice_);

DROP VIEW IF EXISTS android_batt_event;
CREATE VIEW android_batt_event AS
SELECT
  ts,
  dur,
  'Suspended' AS slice_name,
  'Suspend / resume' AS track_name,
  'slice' AS track_type
FROM suspend_slice_
UNION ALL
SELECT ts,
       dur,
       CASE screen_state_val
       WHEN 1 THEN 'Screen off'
       WHEN 2 THEN 'Screen on'
       WHEN 3 THEN 'Always-on display (doze)'
       ELSE 'unknown'
       END AS slice_name,
       'Screen state' AS track_name,
       'slice' AS track_type
FROM screen_state_span
UNION ALL
-- See DeviceIdleController.java for where these states come from and how
-- they transition.
SELECT ts,
       dur,
       CASE doze_light_state_val
       WHEN 0 THEN 'active'
       WHEN 1 THEN 'inactive'
       WHEN 4 THEN 'idle'
       WHEN 5 THEN 'waiting_for_network'
       WHEN 6 THEN 'idle_maintenance'
       WHEN 7 THEN 'override'
       ELSE 'unknown'
       END AS slice_name,
       'Doze light state' AS track_name,
       'slice' AS track_type
FROM doze_light_state_span
UNION ALL
SELECT ts,
       dur,
       CASE doze_deep_state_val
       WHEN 0 THEN 'active'
       WHEN 1 THEN 'inactive'
       WHEN 2 THEN 'idle_pending'
       WHEN 3 THEN 'sensing'
       WHEN 4 THEN 'locating'
       WHEN 5 THEN 'idle'
       WHEN 6 THEN 'idle_maintenance'
       WHEN 7 THEN 'quick_doze_delay'
       ELSE 'unknown'
       END AS slice_name,
       'Doze deep state' AS track_name,
       'slice' AS track_type
FROM doze_deep_state_span
UNION ALL
SELECT ts,
       dur,
       CASE battery_status_val
       -- 0 and 1 are both unknown
       WHEN 2 THEN 'Charging'
       WHEN 3 THEN 'Discharging'
       -- special case when charger is present but battery isn't charging
       WHEN 4 THEN 'Not charging'
       WHEN 5 THEN 'Full'
       ELSE 'unknown'
       END AS slice_name,
       'Charging state' AS track_name,
       'slice' AS track_type
FROM battery_status_span
UNION ALL
SELECT ts,
       dur,
       CASE plug_type_val
       WHEN 0 THEN 'None'
       WHEN 1 THEN 'AC'
       WHEN 2 THEN 'USB'
       WHEN 4 THEN 'Wireless'
       WHEN 8 THEN 'Dock'
       ELSE 'unknown'
       END AS slice_name,
       'Plug type' AS track_name,
       'slice' AS track_type
FROM plug_type_span
UNION ALL
SELECT *
FROM (
  SELECT ts,
         dur,
         value_name AS slice_name,
         CASE track_name
         WHEN 'battery_stats.mobile_radio' THEN 'Cellular interface'
         WHEN 'battery_stats.data_conn' THEN 'Cellular connection'
         WHEN 'battery_stats.phone_signal_strength' THEN 'Cellular strength'
         WHEN 'battery_stats.wifi_radio' THEN 'WiFi interface'
         WHEN 'battery_stats.wifi_suppl' THEN 'Wifi supplicant state'
         WHEN 'battery_stats.wifi_signal_strength' THEN 'WiFi strength'
         ELSE NULL
         END AS track_name,
         'slice' AS track_type
  FROM android_battery_stats_state
)
WHERE track_name IS NOT NULL;

DROP VIEW IF EXISTS android_batt_output;
CREATE VIEW android_batt_output AS
SELECT AndroidBatteryMetric(
  'battery_counters', (
    SELECT RepeatedField(
      AndroidBatteryMetric_BatteryCounters(
        'timestamp_ns', ts,
        'charge_counter_uah', charge_uah,
        'capacity_percent', capacity_percent,
        'current_ua', current_ua,
        'current_avg_ua', current_avg_ua
      )
    )
    FROM android_battery_charge
  ),
  'battery_aggregates', (
    SELECT NULL_IF_EMPTY(AndroidBatteryMetric_BatteryAggregates(
      'total_screen_off_ns',
      SUM(CASE WHEN state = 1.0 AND tbl = 'total' THEN dur ELSE 0 END),
      'total_screen_on_ns',
      SUM(CASE WHEN state = 2.0 AND tbl = 'total' THEN dur ELSE 0 END),
      'total_screen_doze_ns',
      SUM(CASE WHEN state = 3.0 AND tbl = 'total' THEN dur ELSE 0 END),
      'sleep_ns',
      (SELECT SUM(dur) FROM suspend_slice_),
      'sleep_screen_off_ns',
      SUM(CASE WHEN state = 1.0 AND tbl = 'sleep' THEN dur ELSE 0 END),
      'sleep_screen_on_ns',
      SUM(CASE WHEN state = 2.0 AND tbl = 'sleep' THEN dur ELSE 0 END),
      'sleep_screen_doze_ns',
      SUM(CASE WHEN state = 3.0 AND tbl = 'sleep' THEN dur ELSE 0 END),
      'total_wakelock_ns',
      (SELECT SUM(ts_end - ts) FROM android_batt_wakelocks_merged)
      ))
    FROM (
      SELECT dur, screen_state_val AS state, 'total' AS tbl
      FROM screen_state_span
      UNION ALL
      SELECT dur, screen_state_val AS state, 'sleep' AS tbl
      FROM screen_state_span_with_suspend
    )
  ),
  'suspend_period', (
    SELECT RepeatedField(
      AndroidBatteryMetric_SuspendPeriod(
        'timestamp_ns', ts,
        'duration_ns', dur
      )
    )
    FROM suspend_slice_
  )
);
