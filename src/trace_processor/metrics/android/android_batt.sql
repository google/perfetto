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
CREATE VIEW battery_view AS
SELECT
  all_ts.ts as ts,
  current_avg_ua,
  capacity_percent,
  charge_uah,
  current_ua
FROM (
  SELECT distinct(ts) AS ts
  FROM counter c
  JOIN counter_track t on c.track_id = t.id
  WHERE name LIKE 'batt.%'
) AS all_ts
LEFT JOIN (
  SELECT ts, value AS current_avg_ua
  FROM counter c
  JOIN counter_track t on c.track_id = t.id
  WHERE name='batt.current.avg_ua'
) USING(ts)
LEFT JOIN (
  SELECT ts, value AS capacity_percent
  FROM counter c
  JOIN counter_track t on c.track_id = t.id
  WHERE name='batt.capacity_pct'
) USING(ts)
LEFT JOIN (
  SELECT ts, value AS charge_uah
  FROM counter c
  JOIN counter_track t on c.track_id = t.id
  WHERE name='batt.charge_uah'
) USING(ts)
LEFT JOIN (
  SELECT ts, value AS current_ua
  FROM counter c
  JOIN counter_track t on c.track_id = t.id
  WHERE name='batt.current_ua'
) USING(ts)
ORDER BY ts;

DROP TABLE IF EXISTS android_batt_wakelocks_merged;
CREATE TABLE android_batt_wakelocks_merged AS
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
                true
            ) AS new_group
        FROM slice
        WHERE slice.name LIKE 'WakeLock %' AND dur != -1
    )
)
GROUP BY group_id;

CREATE TABLE suspend_slice_ AS
SELECT
       ts,
       lead_ts - ts AS dur
FROM (
    SELECT
       ts,
       start,
       event,
       lag(start) OVER w AS lag_start,
       lag(event) OVER w AS lag_event,
       lead(ts) OVER w AS lead_ts,
       lead(start) OVER w AS lead_start,
       lead(event) OVER w AS lead_event,
       lead(start, 2) OVER w AS lead_2_start,
       lead(event, 2) OVER w AS lead_2_event
    FROM (
        SELECT
               ts,
               EXTRACT_ARG(arg_set_id, 'start') AS start,
               EXTRACT_ARG(arg_set_id, 'val') AS event
        FROM raw
        WHERE name = 'suspend_resume'
    )
    WINDOW w AS (ORDER BY ts)
)
-- We want to find the start and end events with action='timekeeping_freeze'.
-- Unfortunately a bug leads to the action string being lost.
-- In practice, these events always show up in a sequence like the following:
-- start = 1, event = 1     [string would have been 'machine_suspend']
-- start = 1, event = (any) [string would have been 'timekeeping_freeze'] *
--
--                             (sleep happens here)
--
-- start = 0, event = (any) [string would have been 'timekeeping_freeze']
-- start = 0, event = 1     [string would have been 'machine_suspend']
--
-- So we look for this pattern of start and event, anchored on the event marked
-- with "*".
WHERE lag_start = 1 AND lag_event = 1
  AND start = 1
  AND lead_start = 0
  AND lead_2_start = 0 AND lead_2_event = 1;

SELECT RUN_METRIC('android/global_counter_span_view.sql',
  'table_name', 'screen_state',
  'counter_name', 'ScreenState');

CREATE VIRTUAL TABLE screen_state_span_with_suspend
    USING span_join(screen_state_span, suspend_slice_);

CREATE VIEW android_batt_event AS
SELECT
       ts,
       dur,
       'Suspended' AS slice_name,
       'Suspend / resume' AS track_name,
       'slice' AS track_type
FROM suspend_slice_;

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
    FROM battery_view
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
  )
);
