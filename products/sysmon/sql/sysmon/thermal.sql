--
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

INCLUDE PERFETTO MODULE sysmon.pulse;

-- Thermal zones (min / avg / peak °C) across the window.
CREATE PERFETTO FUNCTION sysmon_thermal_zones(
  start_ts LONG,
  end_ts LONG,
  lim LONG
)
RETURNS TABLE(
  zone STRING,
  min_c DOUBLE,
  avg_c DOUBLE,
  peak_c DOUBLE,
  samples LONG
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  t_tracks AS MATERIALIZED (
    SELECT id, name
    FROM counter_track
    WHERE
      type IN (
        'thermal_temperature_sys',
        'thermal_temperature',
        'acpm_thermal_temperature'
      )
      OR name GLOB 'thermal*'
  )
SELECT
  tt.name AS zone,
  round(min(iif(c.value > 200, c.value / 1000.0, c.value)), 1) AS min_c,
  round(avg(iif(c.value > 200, c.value / 1000.0, c.value)), 1) AS avg_c,
  round(max(iif(c.value > 200, c.value / 1000.0, c.value)), 1) AS peak_c,
  count(*) AS samples
FROM t_tracks tt
JOIN counter c ON c.track_id = tt.id
JOIN w
WHERE
  c.ts BETWEEN w.start_ts AND w.end_ts
GROUP BY
  tt.name
ORDER BY
  peak_c DESC
LIMIT coalesce($lim, 15);

-- Thermal zones °C time series (top N hottest zones) at 1s resolution.
CREATE PERFETTO FUNCTION sysmon_thermal_series(
  start_ts LONG,
  end_ts LONG,
  top_n LONG
)
RETURNS TABLE(rel_s DOUBLE, zone STRING, temp_c DOUBLE)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  start_sec AS (
    SELECT w.start_ts / 1000000000 AS s0, w.end_ts / 1000000000 AS s1 FROM w
  ),
  top_zones AS MATERIALIZED (
    SELECT zone
    FROM sysmon_thermal_zones($start_ts, $end_ts, coalesce($top_n, 5))
  ),
  t_tracks AS MATERIALIZED (
    SELECT ct.id, ct.name
    FROM counter_track ct
    JOIN top_zones tz ON tz.zone = ct.name
    WHERE
      ct.type IN (
        'thermal_temperature_sys',
        'thermal_temperature',
        'acpm_thermal_temperature'
      )
      OR ct.name GLOB 'thermal*'
  )
SELECT
  cast_double!(cs.sec - ss.s0) AS rel_s,
  tt.name AS zone,
  round(avg(iif(c.value > 200, c.value / 1000.0, c.value)), 1) AS temp_c
FROM t_tracks tt
JOIN _sysmon_counter_sec cs ON cs.track_id = tt.id
JOIN counter c ON c.id = cs.last_id
JOIN start_sec ss
WHERE
  cs.sec BETWEEN ss.s0 AND ss.s1
  AND cs.sec >= ss.s0
GROUP BY
  cs.sec,
  tt.name
ORDER BY
  cs.sec,
  tt.name;

-- ODPM power rails average power (mW) and total energy (mJ).
CREATE PERFETTO FUNCTION sysmon_power_rails(
  start_ts LONG,
  end_ts LONG,
  lim LONG
)
RETURNS TABLE(rail STRING, avg_mw DOUBLE, energy_mj DOUBLE, samples LONG)
AS
WITH
  wt AS MATERIALIZED (SELECT * FROM _sysmon_window_tracks($start_ts, $end_ts)),
  rails AS (
    SELECT
      ct.name AS rail,
      max(0.0, wt.delta_v) AS delta_uws,
      max(1.0, (wt.max_ts - wt.min_ts) / 1e9) AS span_s,
      wt.cnt AS samples
    FROM counter_track ct
    JOIN wt ON wt.track_id = ct.id
    WHERE
      ct.name GLOB 'power.*'
      AND wt.cnt >= 2
  )
SELECT
  rail,
  round(delta_uws / (span_s * 1000.0), 1) AS avg_mw,
  round(delta_uws / 1000.0, 2) AS energy_mj,
  samples
FROM rails
ORDER BY
  avg_mw DESC
LIMIT coalesce($lim, 15);

-- ODPM power rails instantaneous power (mW) time series at 1s resolution for
-- the top N sub-rails (plus 'Other rails' and, when VSYS_ALL is present,
-- 'Not in a rail' so stacked totals equal whole-phone power without double
-- counting VSYS_ALL).
CREATE PERFETTO FUNCTION sysmon_power_series(
  start_ts LONG,
  end_ts LONG,
  top_n LONG
)
RETURNS TABLE(rel_s DOUBLE, rail STRING, mw DOUBLE)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  start_sec AS (
    SELECT w.start_ts / 1000000000 AS s0, w.end_ts / 1000000000 AS s1 FROM w
  ),
  top_rails AS MATERIALIZED (
    SELECT rail
    FROM sysmon_power_rails($start_ts, $end_ts, coalesce($top_n, 6) + 1)
    WHERE
      NOT (rail GLOB '*VSYS_ALL*')
    LIMIT coalesce($top_n, 6)
  ),
  p_tracks AS MATERIALIZED (
    SELECT
      ct.id,
      iif(ct.name GLOB '*VSYS_ALL*', 1, 0) AS is_vsys,
      coalesce(tr.rail, 'Other rails') AS bucket
    FROM counter_track ct
    LEFT JOIN top_rails tr ON tr.rail = ct.name
    WHERE
      ct.name GLOB 'power.*'
  ),
  step AS MATERIALIZED (
    SELECT
      cs.sec - ss.s0 AS rel_sec,
      pt.is_vsys,
      pt.bucket AS rail,
      max(
        0.0,
        c.value - lag(c.value) OVER (PARTITION BY cs.track_id ORDER BY cs.sec)
      ) AS d_uws,
      (cs.max_ts
      - lag(cs.max_ts) OVER (PARTITION BY cs.track_id ORDER BY cs.sec))
      / 1e9 AS dt_s
    FROM p_tracks pt
    JOIN _sysmon_counter_sec cs ON cs.track_id = pt.id
    JOIN counter c ON c.id = cs.last_id
    JOIN start_sec ss
    WHERE
      cs.sec BETWEEN ss.s0 AND ss.s1
  ),
  sub_rails AS (
    SELECT rel_sec, rail, sum(d_uws / max(0.1, dt_s)) / 1000.0 AS mw
    FROM step
    WHERE
      is_vsys = 0
      AND dt_s IS NOT NULL
      AND dt_s > 0
      AND rel_sec >= 0
    GROUP BY
      rel_sec,
      rail
  ),
  vsys_gap AS (
    SELECT
      v.rel_sec,
      'Not in a rail' AS rail,
      max(0.0, v.tot_mw - coalesce(s.sub_mw, 0.0)) AS mw
    FROM (
      SELECT rel_sec, sum(d_uws / max(0.1, dt_s)) / 1000.0 AS tot_mw
      FROM step
      WHERE
        is_vsys = 1
        AND dt_s IS NOT NULL
        AND dt_s > 0
        AND rel_sec >= 0
      GROUP BY
        rel_sec
    ) v
    LEFT JOIN (
      SELECT rel_sec, sum(mw) AS sub_mw FROM sub_rails GROUP BY rel_sec
    ) s USING (rel_sec)
  )
SELECT cast_double!(rel_sec) AS rel_s, rail, round(mw, 1) AS mw FROM sub_rails
UNION ALL
SELECT cast_double!(rel_sec) AS rel_s, rail, round(mw, 1) AS mw
FROM vsys_gap
WHERE
  mw > 0.5
ORDER BY
  rel_s,
  rail;

-- Active thermal cooling devices from cdev_update ftrace events.
CREATE PERFETTO FUNCTION sysmon_cooling_devices(start_ts LONG, end_ts LONG)
RETURNS TABLE(device STRING, last_state LONG, max_state LONG, updates LONG)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  ev AS (
    SELECT
      fe.ts,
      coalesce(extract_arg(fe.arg_set_id, 'type'), 'cooling') AS device,
      cast_int!(coalesce(extract_arg(fe.arg_set_id, 'target'), 0)) AS target
    FROM ftrace_event fe, w
    WHERE
      EXISTS (
        SELECT 1 FROM _sysmon_ftrace_counts WHERE name = 'cdev_update' LIMIT 1
      )
      AND fe.name = 'cdev_update'
      AND fe.ts BETWEEN w.start_ts AND w.end_ts
  ),
  ranked AS (
    SELECT
      device,
      target,
      row_number() OVER (PARTITION BY device ORDER BY ts DESC) AS rn
    FROM ev
  )
SELECT
  device,
  max(iif(rn = 1, target, 0)) AS last_state,
  max(target) AS max_state,
  count(*) AS updates
FROM ranked
GROUP BY
  device
ORDER BY
  max_state DESC,
  updates DESC
LIMIT 15;
