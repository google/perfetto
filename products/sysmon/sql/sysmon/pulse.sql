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

-- =============================================================================
-- Core Pipeline-SQL materialized rollup tables.
-- Each table runs directly on Perfetto's C++ columnar pipeline executor once
-- per loaded trace, turning O(N) SQLite scans across 15+ queries into O(1)
-- lookups over tiny pre-aggregated tables.
-- =============================================================================

-- Per-track first/last counter row ids and timestamps across the whole trace.
CREATE PERFETTO TABLE _sysmon_counter_bounds AS
FROM counter
|> AGGREGATE
  COUNT(*) AS cnt,
  MIN(id) AS first_id,
  MAX(id) AS last_id,
  MIN(ts) AS min_ts,
  MAX(ts) AS max_ts
GROUP BY
  track_id;

-- Per-track first/last values via O(1) primary-key id lookups on counter.
CREATE PERFETTO TABLE _sysmon_track_summary AS
SELECT
  b.track_id,
  b.cnt,
  b.min_ts,
  b.max_ts,
  c1.value AS first_v,
  c2.value AS last_v,
  c2.value - c1.value AS delta_v
FROM _sysmon_counter_bounds b
JOIN counter c1 ON c1.id = b.first_id
JOIN counter c2 ON c2.id = b.last_id;

CREATE PERFETTO INDEX _sysmon_track_summary_idx ON _sysmon_track_summary(
  track_id
);

-- 1-second bucketed counter endpoints per track for fast time-series & brushed
-- window queries.
CREATE PERFETTO TABLE _sysmon_counter_sec AS
FROM counter
|> EXTEND ts / 1000000000 AS sec
|> AGGREGATE
  COUNT(*) AS cnt,
  MIN(id) AS first_id,
  MAX(id) AS last_id,
  MIN(ts) AS min_ts,
  MAX(ts) AS max_ts
GROUP BY
  track_id,
  sec;

CREATE PERFETTO INDEX _sysmon_counter_sec_idx ON _sysmon_counter_sec(
  track_id,
  sec
);

-- Whole-trace thread_state rollup by (utid, state, io_wait).
CREATE PERFETTO TABLE _sysmon_thread_state_agg AS
FROM thread_state
|> WHERE utid != 0 AND dur > 0
|> AGGREGATE
  COUNT(*) AS cnt,
  SUM(dur) AS total_ns,
  MAX(dur) AS max_ns
GROUP BY
  utid,
  state,
  io_wait;

-- Whole-trace per-CPU sched run time rollup.
CREATE PERFETTO TABLE _sysmon_sched_cpu_agg AS
FROM sched
|> WHERE utid != 0 AND dur > 0
|> AGGREGATE COUNT(*) AS cnt, SUM(dur) AS run_ns GROUP BY cpu;

-- 1-second bucketed sched run time across all CPUs.
CREATE PERFETTO TABLE _sysmon_sched_sec AS
FROM sched
|> WHERE utid != 0 AND dur > 0
|> EXTEND ts / 1000000000 AS sec
|> AGGREGATE SUM(dur) AS run_ns GROUP BY sec;

-- 1-second bucketed per-CPU sched run time for Core x Time heatmaps.
CREATE PERFETTO TABLE _sysmon_sched_cpu_sec AS
FROM sched
|> WHERE utid != 0 AND dur > 0
|> EXTEND ts / 1000000000 AS sec
|> AGGREGATE SUM(dur) AS run_ns GROUP BY cpu, sec;

CREATE PERFETTO INDEX _sysmon_sched_cpu_sec_idx ON _sysmon_sched_cpu_sec(
  sec,
  cpu
);

-- Whole-trace ftrace event counts by event name.
CREATE PERFETTO TABLE _sysmon_ftrace_counts AS
FROM ftrace_event
|> AGGREGATE COUNT(*) AS cnt GROUP BY name;

-- Cached single-row trace capability metadata.
CREATE PERFETTO TABLE _sysmon_trace_meta AS
SELECT
  tb.start_ts,
  tb.end_ts,
  max(1, (SELECT count(*) FROM cpu)) AS cpu_count,
  iif(EXISTS (SELECT 1 FROM _sysmon_sched_cpu_agg LIMIT 1), 1, 0) AS has_sched,
  iif(EXISTS (SELECT 1 FROM counter_track WHERE type = 'cpustat' LIMIT 1), 1, 0) AS has_cpustat,
  iif(EXISTS (SELECT 1 FROM counter_track WHERE type = 'psi' LIMIT 1), 1, 0) AS has_psi,
  iif(EXISTS (SELECT 1 FROM counter_track WHERE type = 'meminfo' LIMIT 1), 1, 0) AS has_meminfo,
  iif(EXISTS (SELECT 1 FROM counter_track WHERE type = 'vmstat' LIMIT 1), 1, 0) AS has_vmstat,
  iif(
    EXISTS (SELECT 1 FROM counter_track WHERE type = 'diskstat' LIMIT 1),
    1,
    0
  ) AS has_diskstat,
  iif(
    EXISTS (
      SELECT 1
      FROM _sysmon_ftrace_counts
      WHERE
        name IN ('block_rq_issue', 'block_rq_complete')
      LIMIT 1
    ),
    1,
    0
  ) AS has_block,
  iif(
    EXISTS (
      SELECT 1
      FROM counter_track
      WHERE
        type IN (
          'thermal_temperature_sys',
          'thermal_temperature',
          'acpm_thermal_temperature'
        )
        OR name GLOB 'thermal*'
      LIMIT 1
    )
    OR EXISTS (
      SELECT 1
      FROM _sysmon_ftrace_counts
      WHERE
        name IN ('thermal_temperature', 'cdev_update')
      LIMIT 1
    ),
    1,
    0
  ) AS has_thermal,
  iif(
    EXISTS (
      SELECT 1
      FROM counter_track
      WHERE
        name GLOB 'power.*'
        OR name GLOB 'batt.*'
      LIMIT 1
    ),
    1,
    0
  ) AS has_power
FROM trace_bounds tb;

-- Windowed track endpoints using the precomputed 1s pipeline buckets (or the
-- whole-trace summary when unbrushed).
CREATE PERFETTO FUNCTION _sysmon_window_tracks(start_ts LONG, end_ts LONG)
RETURNS TABLE(
  track_id LONG,
  cnt LONG,
  min_ts LONG,
  max_ts LONG,
  first_v DOUBLE,
  last_v DOUBLE,
  delta_v DOUBLE
)
AS
WITH
  m AS (SELECT start_ts, end_ts FROM _sysmon_trace_meta),
  sec_bounds AS (
    SELECT
      cs.track_id,
      sum(cs.cnt) AS cnt,
      min(cs.first_id) AS first_id,
      max(cs.last_id) AS last_id,
      min(cs.min_ts) AS min_ts,
      max(cs.max_ts) AS max_ts
    FROM _sysmon_counter_sec cs, m
    WHERE
      ($start_ts IS NOT NULL OR $end_ts IS NOT NULL)
      AND cs.sec BETWEEN coalesce($start_ts, m.start_ts) / 1000000000 AND coalesce(
        $end_ts,
        m.end_ts
      )
      / 1000000000
    GROUP BY
      cs.track_id
  )
SELECT track_id, cnt, min_ts, max_ts, first_v, last_v, delta_v
FROM _sysmon_track_summary
WHERE
  $start_ts IS NULL
  AND $end_ts IS NULL
UNION ALL
SELECT
  b.track_id,
  b.cnt,
  b.min_ts,
  b.max_ts,
  c1.value AS first_v,
  c2.value AS last_v,
  c2.value - c1.value AS delta_v
FROM sec_bounds b
JOIN counter c1 ON c1.id = b.first_id
JOIN counter c2 ON c2.id = b.last_id;

-- Resolved analysis window and signal-availability flags so the UI and agent
-- never confuse unsupported counters with 0.
CREATE PERFETTO FUNCTION sysmon_window_bounds(start_ts LONG, end_ts LONG)
RETURNS TABLE(
  start_ts LONG,
  end_ts LONG,
  dur_s DOUBLE,
  cpu_count LONG,
  has_sched LONG,
  has_cpustat LONG,
  has_psi LONG,
  has_meminfo LONG,
  has_vmstat LONG,
  has_diskstat LONG,
  has_block LONG,
  has_thermal LONG,
  has_power LONG
)
AS
SELECT
  coalesce($start_ts, m.start_ts) AS start_ts,
  coalesce($end_ts, m.end_ts) AS end_ts,
  max(
    0.001,
    (coalesce($end_ts, m.end_ts) - coalesce($start_ts, m.start_ts)) / 1e9
  ) AS dur_s,
  m.cpu_count,
  m.has_sched,
  m.has_cpustat,
  m.has_psi,
  m.has_meminfo,
  m.has_vmstat,
  m.has_diskstat,
  m.has_block,
  m.has_thermal,
  m.has_power
FROM _sysmon_trace_meta m;

-- Helper: PSI stall percentage over [start_ts, end_ts] for a psi.* track.
CREATE PERFETTO FUNCTION sysmon_psi_pct(
  track_name STRING,
  start_ts LONG,
  end_ts LONG
)
RETURNS DOUBLE
AS
SELECT
  CASE
    WHEN wt.cnt >= 2
    AND (wt.max_ts - wt.min_ts) > 0 THEN round(
      100.0 * max(0.0, wt.delta_v) / (wt.max_ts - wt.min_ts),
      2
    )
    ELSE NULL
  END
FROM counter_track ct
JOIN _sysmon_window_tracks($start_ts, $end_ts) wt ON wt.track_id = ct.id
WHERE
  ct.type = 'psi'
  AND ct.name = $track_name
LIMIT 1;

-- Single-row whole-device pulse summary covering CPU, Memory, Storage, Thermal.
CREATE PERFETTO FUNCTION sysmon_pulse(start_ts LONG, end_ts LONG)
RETURNS TABLE(
  start_ts LONG,
  end_ts LONG,
  dur_s DOUBLE,
  cpu_count LONG,
  cpu_busy_pct DOUBLE,
  cpu_iowait_pct DOUBLE,
  cpu_psi_some_pct DOUBLE,
  mem_total_bytes DOUBLE,
  mem_avail_min_bytes DOUBLE,
  mem_avail_last_bytes DOUBLE,
  mem_anon_bytes DOUBLE,
  mem_cache_bytes DOUBLE,
  mem_dmabuf_gpu_bytes DOUBLE,
  mem_zram_bytes DOUBLE,
  mem_psi_some_pct DOUBLE,
  mem_psi_full_pct DOUBLE,
  lmkd_kills LONG,
  direct_reclaim_events LONG,
  io_psi_some_pct DOUBLE,
  io_psi_full_pct DOUBLE,
  io_read_mb DOUBLE,
  io_write_mb DOUBLE,
  io_dstate_ms DOUBLE,
  peak_temp_c DOUBLE,
  throttle_events LONG,
  avg_power_mw DOUBLE,
  batt_pct DOUBLE,
  batt_current_ma DOUBLE,
  batt_voltage_v DOUBLE,
  has_sched LONG,
  has_psi LONG,
  has_meminfo LONG,
  has_diskstat LONG,
  has_block LONG,
  has_thermal LONG,
  has_power LONG
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  unbrushed AS (SELECT iif($start_ts IS NULL AND $end_ts IS NULL, 1, 0) AS flag),
  wt AS MATERIALIZED (SELECT * FROM _sysmon_window_tracks($start_ts, $end_ts)),
  cpustat_agg AS (
    SELECT
      sum(max(0.0, wt.delta_v)) AS tot_ns,
      sum(
        iif(
          ct.name NOT IN ('cpu.times.idle_ns', 'cpu.times.io_wait_ns'),
          max(0.0, wt.delta_v),
          0.0
        )
      ) AS busy_ns,
      sum(iif(ct.name = 'cpu.times.io_wait_ns', max(0.0, wt.delta_v), 0.0)) AS iowait_ns
    FROM counter_track ct
    JOIN wt ON wt.track_id = ct.id
    WHERE
      ct.type = 'cpustat'
  ),
  sched_cpu AS (
    SELECT
      coalesce(
        iif(
          (SELECT flag FROM unbrushed) = 1,
          (SELECT sum(run_ns) FROM _sysmon_sched_cpu_agg),
          (
            SELECT sum(run_ns)
            FROM _sysmon_sched_sec, w
            WHERE
              sec BETWEEN w.start_ts / 1000000000 AND w.end_ts / 1000000000
          )
        ),
        0
      ) AS run_ns
  ),
  mem_agg AS (
    SELECT
      max(iif(ct.name = 'MemTotal', wt.last_v, NULL)) AS mem_total,
      max(iif(ct.name = 'MemAvailable', wt.last_v, NULL)) AS mem_avail_last,
      max(iif(ct.name = 'AnonPages', wt.last_v, NULL)) AS mem_anon,
      coalesce(max(iif(ct.name = 'Cached', wt.last_v, NULL)), 0)
      + coalesce(max(iif(ct.name = 'Buffers', wt.last_v, NULL)), 0) AS mem_cache,
      coalesce(max(iif(ct.name = 'ION_heap', wt.last_v, NULL)), 0)
      + coalesce(max(iif(ct.name = 'ION_heap_pool', wt.last_v, NULL)), 0)
      + coalesce(max(iif(ct.name = 'Gpu', wt.last_v, NULL)), 0) AS mem_dma_gpu,
      max(iif(ct.name = 'Zram', wt.last_v, NULL)) AS mem_zram
    FROM counter_track ct
    JOIN wt ON wt.track_id = ct.id
    WHERE
      ct.type = 'meminfo'
  ),
  mem_avail_min AS (
    SELECT min(c.value) AS min_v
    FROM counter c
    JOIN counter_track ct ON c.track_id = ct.id
    JOIN w
    WHERE
      ct.type = 'meminfo'
      AND ct.name = 'MemAvailable'
      AND c.ts BETWEEN w.start_ts AND w.end_ts
  ),
  dma_extra AS (
    SELECT max(wt.last_v) AS dma_gpu_track_bytes
    FROM counter_track ct
    JOIN wt ON wt.track_id = ct.id
    WHERE
      ct.type IN ('android_dma_heap', 'gpu_memory')
  ),
  psi_agg AS (
    SELECT
      max(
        iif(
          ct.name = 'psi.cpu.some'
          AND wt.cnt >= 2
          AND wt.max_ts > wt.min_ts,
          round(100.0 * max(0.0, wt.delta_v) / (wt.max_ts - wt.min_ts), 2),
          NULL
        )
      ) AS cpu_some,
      max(
        iif(
          ct.name = 'psi.mem.some'
          AND wt.cnt >= 2
          AND wt.max_ts > wt.min_ts,
          round(100.0 * max(0.0, wt.delta_v) / (wt.max_ts - wt.min_ts), 2),
          NULL
        )
      ) AS mem_some,
      max(
        iif(
          ct.name = 'psi.mem.full'
          AND wt.cnt >= 2
          AND wt.max_ts > wt.min_ts,
          round(100.0 * max(0.0, wt.delta_v) / (wt.max_ts - wt.min_ts), 2),
          NULL
        )
      ) AS mem_full,
      max(
        iif(
          ct.name = 'psi.io.some'
          AND wt.cnt >= 2
          AND wt.max_ts > wt.min_ts,
          round(100.0 * max(0.0, wt.delta_v) / (wt.max_ts - wt.min_ts), 2),
          NULL
        )
      ) AS io_some,
      max(
        iif(
          ct.name = 'psi.io.full'
          AND wt.cnt >= 2
          AND wt.max_ts > wt.min_ts,
          round(100.0 * max(0.0, wt.delta_v) / (wt.max_ts - wt.min_ts), 2),
          NULL
        )
      ) AS io_full
    FROM counter_track ct
    JOIN wt ON wt.track_id = ct.id
    WHERE
      ct.type = 'psi'
  ),
  lmk AS (
    SELECT
      iif(
        (SELECT flag FROM unbrushed) = 1,
        coalesce(
          (
            SELECT sum(cnt)
            FROM _sysmon_ftrace_counts
            WHERE
              name IN ('lowmemory_kill', 'mark_victim')
          ),
          0
        ),
        (
          SELECT count(*)
          FROM ftrace_event fe, w
          WHERE
            fe.name IN ('lowmemory_kill', 'mark_victim')
            AND fe.ts BETWEEN w.start_ts AND w.end_ts
        )
      ) AS kills
  ),
  reclaim AS (
    SELECT
      iif(
        (SELECT flag FROM unbrushed) = 1,
        coalesce(
          (
            SELECT sum(cnt)
            FROM _sysmon_ftrace_counts
            WHERE
              name = 'mm_vmscan_direct_reclaim_begin'
          ),
          0
        ),
        (
          SELECT count(*)
          FROM ftrace_event fe, w
          WHERE
            fe.name = 'mm_vmscan_direct_reclaim_begin'
            AND fe.ts BETWEEN w.start_ts AND w.end_ts
        )
      ) AS cnt
  ),
  disk_tracks AS MATERIALIZED (
    SELECT id, name
    FROM counter_track
    WHERE
      type = 'diskstat'
      AND (name GLOB 'diskstat.*.read_amount'
      OR name GLOB 'diskstat.*.write_amount')
  ),
  disk_io AS (
    SELECT
      sum(iif(dt.name GLOB 'diskstat.*.read_amount', c.value, 0.0)) AS read_mb,
      sum(iif(dt.name GLOB 'diskstat.*.write_amount', c.value, 0.0)) AS write_mb
    FROM disk_tracks dt
    JOIN counter c ON c.track_id = dt.id
    JOIN w
    WHERE
      c.ts BETWEEN w.start_ts AND w.end_ts
  ),
  block_io AS (
    SELECT
      iif(
        w.has_diskstat = 1
        OR w.has_block = 0,
        NULL,
        (
          SELECT
            sum(
              iif(
                coalesce(extract_arg(fe.arg_set_id, 'rwbs'), '') GLOB '*W*',
                0.0,
                coalesce(extract_arg(fe.arg_set_id, 'nr_sector'), 0) * 512.0
                / 1048576.0
              )
            )
          FROM ftrace_event fe
          WHERE
            fe.name = 'block_rq_issue'
            AND fe.ts BETWEEN w.start_ts AND w.end_ts
        )
      ) AS read_mb,
      iif(
        w.has_diskstat = 1
        OR w.has_block = 0,
        NULL,
        (
          SELECT
            sum(
              iif(
                coalesce(extract_arg(fe.arg_set_id, 'rwbs'), '') GLOB '*W*',
                coalesce(extract_arg(fe.arg_set_id, 'nr_sector'), 0) * 512.0
                / 1048576.0,
                0.0
              )
            )
          FROM ftrace_event fe
          WHERE
            fe.name = 'block_rq_issue'
            AND fe.ts BETWEEN w.start_ts AND w.end_ts
        )
      ) AS write_mb
    FROM w
  ),
  dstate AS (
    SELECT
      iif(
        (SELECT flag FROM unbrushed) = 1,
        coalesce(
          (
            SELECT sum(total_ns)
            FROM _sysmon_thread_state_agg
            WHERE
              state = 'D'
              AND io_wait = 1
          ),
          0
        )
        / 1e6,
        coalesce(
          (
            SELECT sum(max(0, min(ts + dur, w.end_ts) - max(ts, w.start_ts)))
            FROM thread_state, w
            WHERE
              state = 'D'
              AND io_wait = 1
              AND dur > 0
              AND ts < w.end_ts
              AND ts + dur > w.start_ts
          ),
          0
        )
        / 1e6
      ) AS io_ms
  ),
  therm_tracks AS MATERIALIZED (
    SELECT id
    FROM counter_track
    WHERE
      type IN (
        'thermal_temperature_sys',
        'thermal_temperature',
        'acpm_thermal_temperature'
      )
      OR name GLOB 'thermal*'
  ),
  therm AS (
    SELECT max(iif(c.value > 200, c.value / 1000.0, c.value)) AS peak_c
    FROM therm_tracks tt
    JOIN counter c ON c.track_id = tt.id
    JOIN w
    WHERE
      c.ts BETWEEN w.start_ts AND w.end_ts
  ),
  cdev AS (
    SELECT
      iif(
        (SELECT flag FROM unbrushed) = 1,
        coalesce(
          (
            SELECT sum(cnt)
            FROM _sysmon_ftrace_counts
            WHERE
              name = 'cdev_update'
          ),
          0
        ),
        (
          SELECT count(*)
          FROM ftrace_event fe, w
          WHERE
            fe.name = 'cdev_update'
            AND fe.ts BETWEEN w.start_ts AND w.end_ts
        )
      ) AS cnt
  ),
  pwr AS (
    SELECT
      CASE
        WHEN max(iif(ct.name GLOB '*VSYS_ALL*', 1, 0)) = 1 THEN sum(
          iif(ct.name GLOB '*VSYS_ALL*', max(0.0, wt.delta_v), 0.0)
        )
        / (max(1.0, (SELECT dur_s FROM w)) * 1000.0)
        ELSE sum(max(0.0, wt.delta_v))
        / (max(1.0, (SELECT dur_s FROM w)) * 1000.0)
      END AS mw
    FROM counter_track ct
    JOIN wt ON wt.track_id = ct.id
    WHERE
      ct.name GLOB 'power.*'
      AND wt.cnt >= 2
  ),
  batt AS (
    SELECT
      max(iif(ct.name = 'batt.capacity_pct', wt.last_v, NULL)) AS pct,
      max(
        iif(
          ct.name IN ('batt.current_ua', 'batt.current.avg_ua'),
          wt.last_v / 1000.0,
          NULL
        )
      ) AS current_ma,
      max(iif(ct.name = 'batt.voltage_uv', wt.last_v / 1e6, NULL)) AS voltage_v
    FROM counter_track ct
    JOIN wt ON wt.track_id = ct.id
    WHERE
      ct.name GLOB 'batt.*'
  )
SELECT
  w.start_ts,
  w.end_ts,
  round(w.dur_s, 2) AS dur_s,
  w.cpu_count,
  CASE
    WHEN ca.tot_ns > 0 THEN round(100.0 * ca.busy_ns / ca.tot_ns, 1)
    WHEN w.has_sched = 1 THEN round(
      min(100.0, 100.0 * coalesce(sc.run_ns, 0) / (w.dur_s * 1e9 * w.cpu_count)),
      1
    )
    ELSE NULL
  END AS cpu_busy_pct,
  CASE
    WHEN ca.tot_ns > 0 THEN round(100.0 * ca.iowait_ns / ca.tot_ns, 1)
    ELSE NULL
  END AS cpu_iowait_pct,
  pa.cpu_some AS cpu_psi_some_pct,
  ma.mem_total AS mem_total_bytes,
  coalesce(mam.min_v, ma.mem_avail_last) AS mem_avail_min_bytes,
  ma.mem_avail_last AS mem_avail_last_bytes,
  ma.mem_anon AS mem_anon_bytes,
  iif(w.has_meminfo = 1, ma.mem_cache, NULL) AS mem_cache_bytes,
  CASE
    WHEN ma.mem_dma_gpu > 0 THEN ma.mem_dma_gpu
    WHEN de.dma_gpu_track_bytes IS NOT NULL THEN de.dma_gpu_track_bytes
    ELSE iif(w.has_meminfo = 1, 0.0, NULL)
  END AS mem_dmabuf_gpu_bytes,
  ma.mem_zram AS mem_zram_bytes,
  pa.mem_some AS mem_psi_some_pct,
  pa.mem_full AS mem_psi_full_pct,
  lmk.kills AS lmkd_kills,
  reclaim.cnt AS direct_reclaim_events,
  pa.io_some AS io_psi_some_pct,
  pa.io_full AS io_psi_full_pct,
  round(
    coalesce(
      iif(w.has_diskstat = 1, disk_io.read_mb, NULL),
      block_io.read_mb,
      0.0
    ),
    2
  ) AS io_read_mb,
  round(
    coalesce(
      iif(w.has_diskstat = 1, disk_io.write_mb, NULL),
      block_io.write_mb,
      0.0
    ),
    2
  ) AS io_write_mb,
  round(dstate.io_ms, 1) AS io_dstate_ms,
  round(therm.peak_c, 1) AS peak_temp_c,
  cdev.cnt AS throttle_events,
  round(pwr.mw, 1) AS avg_power_mw,
  round(batt.pct, 0) AS batt_pct,
  round(batt.current_ma, 0) AS batt_current_ma,
  round(batt.voltage_v, 2) AS batt_voltage_v,
  w.has_sched,
  w.has_psi,
  w.has_meminfo,
  w.has_diskstat,
  w.has_block,
  w.has_thermal,
  w.has_power
FROM w, cpustat_agg ca, sched_cpu sc, mem_agg ma, mem_avail_min mam, dma_extra de, psi_agg pa, lmk, reclaim, disk_io, block_io, dstate, therm, cdev, pwr, batt;

-- Synchronized N-bin pressure strip for the brushable timeline on Start.
CREATE PERFETTO FUNCTION sysmon_pressure_buckets(
  start_ts LONG,
  end_ts LONG,
  num_bins LONG
)
RETURNS TABLE(
  bin LONG,
  rel_s DOUBLE,
  cpu_pressure DOUBLE,
  mem_pressure DOUBLE,
  io_pressure DOUBLE,
  thermal_pressure DOUBLE,
  has_lmkd LONG
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  nb AS (SELECT max(4, min(120, coalesce($num_bins, 48))) AS n),
  bins(bin) AS (
    SELECT 0
    UNION ALL
    SELECT bin + 1 FROM bins, nb WHERE bin + 1 < nb.n
  ),
  psi_tracks AS MATERIALIZED (
    SELECT id, name
    FROM counter_track
    WHERE
      type = 'psi'
      AND name IN ('psi.cpu.some', 'psi.mem.some', 'psi.io.some')
  ),
  psi_pts AS (
    SELECT
      pt.name,
      cs.max_ts AS ts,
      c.value - lag(c.value) OVER (PARTITION BY cs.track_id ORDER BY cs.sec) AS dv,
      cs.max_ts - lag(cs.max_ts) OVER (PARTITION BY cs.track_id ORDER BY cs.sec) AS dt
    FROM psi_tracks pt
    JOIN _sysmon_counter_sec cs ON cs.track_id = pt.id
    JOIN counter c ON c.id = cs.last_id
    JOIN w
    WHERE
      cs.sec BETWEEN w.start_ts / 1000000000 AND w.end_ts / 1000000000
  ),
  psi_binned AS (
    SELECT
      min(
        nb.n - 1,
        max(
          0,
          cast_int!((ts - w.start_ts) * nb.n / max(1, w.end_ts - w.start_ts))
        )
      ) AS bin,
      max(
        iif(name = 'psi.cpu.some' AND dt > 0, min(1.0, max(0.0, dv / dt)), 0.0)
      ) AS cpu_p,
      max(
        iif(name = 'psi.mem.some' AND dt > 0, min(1.0, max(0.0, dv / dt)), 0.0)
      ) AS mem_p,
      max(
        iif(name = 'psi.io.some' AND dt > 0, min(1.0, max(0.0, dv / dt)), 0.0)
      ) AS io_p
    FROM psi_pts, w, nb
    WHERE
      dt IS NOT NULL
      AND dt > 0
    GROUP BY
      1
  ),
  sched_binned AS (
    SELECT
      min(
        nb.n - 1,
        max(
          0,
          cast_int!(
            ((ss.sec * 1000000000) - w.start_ts) * nb.n
            / max(1, w.end_ts - w.start_ts)
          )
        )
      ) AS bin,
      min(
        1.0,
        sum(ss.run_ns)
        / max(1.0, ((w.end_ts - w.start_ts) / nb.n) * w.cpu_count)
      ) AS cpu_busy
    FROM _sysmon_sched_sec ss, w, nb
    WHERE
      ss.sec BETWEEN w.start_ts / 1000000000 AND w.end_ts / 1000000000
    GROUP BY
      1
  ),
  block_binned AS (
    SELECT
      min(
        nb.n - 1,
        max(
          0,
          cast_int!((fe.ts - w.start_ts) * nb.n / max(1, w.end_ts - w.start_ts))
        )
      ) AS bin,
      min(1.0, count(*) / 120.0) AS io_p
    FROM ftrace_event fe, w, nb
    WHERE
      w.has_block = 1
      AND fe.name = 'block_rq_issue'
      AND fe.ts BETWEEN w.start_ts AND w.end_ts
    GROUP BY
      1
  ),
  mem_total_val AS MATERIALIZED (
    SELECT max(ts.last_v) AS total_bytes
    FROM counter_track ct
    JOIN _sysmon_track_summary ts ON ts.track_id = ct.id
    WHERE
      ct.type = 'meminfo'
      AND ct.name = 'MemTotal'
  ),
  mem_avail_track AS MATERIALIZED (
    SELECT id
    FROM counter_track
    WHERE
      type = 'meminfo'
      AND name = 'MemAvailable'
  ),
  mem_binned AS (
    SELECT
      min(
        nb.n - 1,
        max(
          0,
          cast_int!(
            (cs.max_ts - w.start_ts) * nb.n / max(1, w.end_ts - w.start_ts)
          )
        )
      ) AS bin,
      max(1.0 - min(1.0, c.value / max(1.0, mt.total_bytes))) AS used_frac
    FROM mem_avail_track mat
    JOIN _sysmon_counter_sec cs ON cs.track_id = mat.id
    JOIN counter c ON c.id = cs.last_id
    JOIN w, nb, mem_total_val mt
    WHERE
      cs.sec BETWEEN w.start_ts / 1000000000 AND w.end_ts / 1000000000
    GROUP BY
      1
  ),
  therm_tracks AS MATERIALIZED (
    SELECT id
    FROM counter_track
    WHERE
      type IN (
        'thermal_temperature_sys',
        'thermal_temperature',
        'acpm_thermal_temperature'
      )
      OR name GLOB 'thermal*'
  ),
  therm_binned AS (
    SELECT
      min(
        nb.n - 1,
        max(
          0,
          cast_int!(
            (cs.max_ts - w.start_ts) * nb.n / max(1, w.end_ts - w.start_ts)
          )
        )
      ) AS bin,
      min(
        1.0,
        max(0.0, (iif(c.value > 200, c.value / 1000.0, c.value) - 32.0) / 28.0)
      ) AS t_p
    FROM therm_tracks tt
    JOIN _sysmon_counter_sec cs ON cs.track_id = tt.id
    JOIN counter c ON c.id = cs.last_id
    JOIN w, nb
    WHERE
      cs.sec BETWEEN w.start_ts / 1000000000 AND w.end_ts / 1000000000
    GROUP BY
      1
  ),
  lmk_binned AS (
    SELECT
      min(
        nb.n - 1,
        max(
          0,
          cast_int!((fe.ts - w.start_ts) * nb.n / max(1, w.end_ts - w.start_ts))
        )
      ) AS bin,
      1 AS hit
    FROM ftrace_event fe, w, nb
    WHERE
      EXISTS (
        SELECT 1
        FROM _sysmon_ftrace_counts
        WHERE
          name IN ('lowmemory_kill', 'mark_victim')
        LIMIT 1
      )
      AND fe.name IN ('lowmemory_kill', 'mark_victim')
      AND fe.ts BETWEEN w.start_ts AND w.end_ts
    GROUP BY
      1
  )
SELECT
  b.bin,
  round((b.bin + 0.5) * w.dur_s / nb.n, 2) AS rel_s,
  round(max(coalesce(pb.cpu_p, 0.0), coalesce(sb.cpu_busy, 0.0)), 3) AS cpu_pressure,
  round(
    max(
      coalesce(pb.mem_p, 0.0),
      max(0.0, (coalesce(mb.used_frac, 0.0) - 0.55) / 0.45)
    ),
    3
  ) AS mem_pressure,
  round(max(coalesce(pb.io_p, 0.0), coalesce(bb.io_p, 0.0)), 3) AS io_pressure,
  round(coalesce(tb.t_p, 0.0), 3) AS thermal_pressure,
  coalesce(lb.hit, 0) AS has_lmkd
FROM bins b
JOIN w, nb
LEFT JOIN psi_binned pb USING (bin)
LEFT JOIN sched_binned sb USING (bin)
LEFT JOIN block_binned bb USING (bin)
LEFT JOIN mem_binned mb USING (bin)
LEFT JOIN therm_binned tb USING (bin)
LEFT JOIN lmk_binned lb USING (bin)
ORDER BY
  b.bin;
