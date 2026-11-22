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

-- Per-CPU utilization and frequency summary (works from cpustat or sched).
CREATE PERFETTO FUNCTION sysmon_cpu_cores(start_ts LONG, end_ts LONG)
RETURNS TABLE(
  cpu LONG,
  busy_pct DOUBLE,
  user_pct DOUBLE,
  sys_pct DOUBLE,
  iowait_pct DOUBLE,
  irq_pct DOUBLE,
  avg_freq_khz DOUBLE,
  max_freq_khz DOUBLE
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  wt AS MATERIALIZED (SELECT * FROM _sysmon_window_tracks($start_ts, $end_ts)),
  cs AS (
    SELECT
      ct.cpu,
      sum(max(0.0, wt.delta_v)) AS tot,
      sum(
        iif(
          ct.name IN ('cpu.times.user_ns', 'cpu.times.user_nice_ns'),
          max(0.0, wt.delta_v),
          0.0
        )
      ) AS usr,
      sum(iif(ct.name = 'cpu.times.system_mode_ns', max(0.0, wt.delta_v), 0.0)) AS sys,
      sum(iif(ct.name = 'cpu.times.io_wait_ns', max(0.0, wt.delta_v), 0.0)) AS iow,
      sum(
        iif(
          ct.name IN ('cpu.times.irq_ns', 'cpu.times.softirq_ns'),
          max(0.0, wt.delta_v),
          0.0
        )
      ) AS irq,
      sum(
        iif(
          ct.name NOT IN ('cpu.times.idle_ns', 'cpu.times.io_wait_ns'),
          max(0.0, wt.delta_v),
          0.0
        )
      ) AS busy
    FROM cpu_counter_track ct
    JOIN wt ON wt.track_id = ct.id
    WHERE
      ct.type = 'cpustat'
    GROUP BY
      ct.cpu
  ),
  sc AS (
    SELECT cpu, run_ns
    FROM _sysmon_sched_cpu_agg
    WHERE
      $start_ts IS NULL
      AND $end_ts IS NULL
    UNION ALL
    SELECT
      s.cpu,
      sum(max(0, min(s.ts + s.dur, w.end_ts) - max(s.ts, w.start_ts))) AS run_ns
    FROM sched s, w
    WHERE
      ($start_ts IS NOT NULL OR $end_ts IS NOT NULL)
      AND s.utid != 0
      AND s.dur > 0
      AND s.ts < w.end_ts
      AND s.ts + s.dur > w.start_ts
    GROUP BY
      s.cpu
  ),
  freq_tracks AS MATERIALIZED (
    SELECT id, cpu FROM cpu_counter_track WHERE type = 'cpu_frequency'
  ),
  fr AS (
    SELECT ft.cpu, avg(c.value) AS avg_khz, max(c.value) AS max_khz
    FROM freq_tracks ft
    JOIN _sysmon_counter_sec cs ON cs.track_id = ft.id
    JOIN counter c ON c.id = cs.last_id
    JOIN w
    WHERE
      cs.sec BETWEEN w.start_ts / 1000000000 AND w.end_ts / 1000000000
    GROUP BY
      ft.cpu
  ),
  all_cpus AS (
    SELECT cpu FROM cs
    UNION
    SELECT cpu FROM sc
    UNION
    SELECT cpu FROM fr
  )
SELECT
  ac.cpu,
  CASE
    WHEN cs.tot > 0 THEN round(100.0 * cs.busy / cs.tot, 1)
    WHEN sc.run_ns IS NOT NULL THEN round(
      min(100.0, 100.0 * sc.run_ns / (w.dur_s * 1e9)),
      1
    )
    ELSE NULL
  END AS busy_pct,
  iif(cs.tot > 0, round(100.0 * cs.usr / cs.tot, 1), NULL) AS user_pct,
  iif(cs.tot > 0, round(100.0 * cs.sys / cs.tot, 1), NULL) AS sys_pct,
  iif(cs.tot > 0, round(100.0 * cs.iow / cs.tot, 1), NULL) AS iowait_pct,
  iif(cs.tot > 0, round(100.0 * cs.irq / cs.tot, 1), NULL) AS irq_pct,
  round(fr.avg_khz, 0) AS avg_freq_khz,
  round(fr.max_khz, 0) AS max_freq_khz
FROM all_cpus ac
JOIN w
LEFT JOIN cs ON cs.cpu = ac.cpu
LEFT JOIN sc ON sc.cpu = ac.cpu
LEFT JOIN fr ON fr.cpu = ac.cpu
ORDER BY
  ac.cpu;

-- CPU breakdown time series (busy, user, sys, iowait, irq, CPU PSI %) at 1s
-- resolution from precomputed pipeline buckets.
CREATE PERFETTO FUNCTION sysmon_cpu_series(start_ts LONG, end_ts LONG)
RETURNS TABLE(
  rel_s DOUBLE,
  busy_pct DOUBLE,
  user_pct DOUBLE,
  sys_pct DOUBLE,
  iowait_pct DOUBLE,
  irq_pct DOUBLE,
  psi_cpu_pct DOUBLE
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  start_sec AS (
    SELECT w.start_ts / 1000000000 AS s0, w.end_ts / 1000000000 AS s1 FROM w
  ),
  cs_tracks AS MATERIALIZED (
    SELECT id, name FROM counter_track WHERE type = 'cpustat'
  ),
  cs_step AS (
    SELECT
      cs.sec - ss.s0 AS rel_sec,
      ct.name,
      c.value - lag(c.value) OVER (PARTITION BY cs.track_id ORDER BY cs.sec) AS dv
    FROM cs_tracks ct
    JOIN _sysmon_counter_sec cs ON cs.track_id = ct.id
    JOIN counter c ON c.id = cs.last_id
    JOIN start_sec ss
    WHERE
      cs.sec BETWEEN ss.s0 AND ss.s1
  ),
  cs_sec AS (
    SELECT
      rel_sec,
      sum(dv) AS tot,
      sum(
        iif(name NOT IN ('cpu.times.idle_ns', 'cpu.times.io_wait_ns'), dv, 0.0)
      ) AS busy,
      sum(iif(name IN ('cpu.times.user_ns', 'cpu.times.user_nice_ns'), dv, 0.0)) AS usr,
      sum(iif(name = 'cpu.times.system_mode_ns', dv, 0.0)) AS sys,
      sum(iif(name = 'cpu.times.io_wait_ns', dv, 0.0)) AS iow,
      sum(iif(name IN ('cpu.times.irq_ns', 'cpu.times.softirq_ns'), dv, 0.0)) AS irq
    FROM cs_step
    WHERE
      dv IS NOT NULL
      AND dv >= 0
    GROUP BY
      rel_sec
  ),
  sc_sec AS (
    SELECT
      ss_row.sec - ss.s0 AS rel_sec,
      min(100.0, 100.0 * ss_row.run_ns / (1e9 * w.cpu_count)) AS busy_pct
    FROM _sysmon_sched_sec ss_row, w, start_sec ss
    WHERE
      ss_row.sec BETWEEN ss.s0 AND ss.s1
  ),
  psi_track AS MATERIALIZED (
    SELECT id FROM counter_track WHERE type = 'psi' AND name = 'psi.cpu.some'
  ),
  psi_step AS (
    SELECT
      cs.sec - ss.s0 AS rel_sec,
      100.0 * max(0.0, c.value - lag(c.value) OVER (ORDER BY cs.sec))
      / max(1.0, cs.max_ts - lag(cs.max_ts) OVER (ORDER BY cs.sec)) AS psi_pct
    FROM psi_track pt
    JOIN _sysmon_counter_sec cs ON cs.track_id = pt.id
    JOIN counter c ON c.id = cs.last_id
    JOIN start_sec ss
    WHERE
      cs.sec BETWEEN ss.s0 AND ss.s1
  ),
  all_secs AS (
    SELECT rel_sec FROM cs_sec
    UNION
    SELECT rel_sec FROM sc_sec
    UNION
    SELECT rel_sec FROM psi_step WHERE psi_pct IS NOT NULL
  )
SELECT
  cast_double!(a.rel_sec) AS rel_s,
  round(
    coalesce(iif(cs.tot > 0, 100.0 * cs.busy / cs.tot, NULL), sc.busy_pct, 0.0),
    1
  ) AS busy_pct,
  round(
    coalesce(iif(cs.tot > 0, 100.0 * cs.usr / cs.tot, NULL), sc.busy_pct, 0.0),
    1
  ) AS user_pct,
  round(coalesce(iif(cs.tot > 0, 100.0 * cs.sys / cs.tot, NULL), 0.0), 1) AS sys_pct,
  round(coalesce(iif(cs.tot > 0, 100.0 * cs.iow / cs.tot, NULL), 0.0), 1) AS iowait_pct,
  round(coalesce(iif(cs.tot > 0, 100.0 * cs.irq / cs.tot, NULL), 0.0), 1) AS irq_pct,
  round(coalesce(ps.psi_pct, 0.0), 1) AS psi_cpu_pct
FROM all_secs a
LEFT JOIN cs_sec cs USING (rel_sec)
LEFT JOIN sc_sec sc USING (rel_sec)
LEFT JOIN psi_step ps USING (rel_sec)
WHERE
  a.rel_sec >= 0
ORDER BY
  a.rel_sec;

-- CPU frequency time series (MHz) grouped into readable core clusters
-- (e.g. Little / Mid / Big on heterogeneous mobile SoCs, or CPU 0..N).
CREATE PERFETTO FUNCTION sysmon_cpu_freq_series(start_ts LONG, end_ts LONG)
RETURNS TABLE(rel_s DOUBLE, cluster STRING, mhz DOUBLE)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  start_sec AS (
    SELECT w.start_ts / 1000000000 AS s0, w.end_ts / 1000000000 AS s1 FROM w
  ),
  freq_tracks AS MATERIALIZED (
    SELECT id, cpu FROM cpu_counter_track WHERE type = 'cpu_frequency'
  ),
  cpu_cnt AS (SELECT count(*) AS n FROM freq_tracks),
  per_cpu_sec AS (
    SELECT ft.cpu, cs.sec - ss.s0 AS rel_sec, c.value / 1000.0 AS mhz
    FROM freq_tracks ft
    JOIN _sysmon_counter_sec cs ON cs.track_id = ft.id
    JOIN counter c ON c.id = cs.last_id
    JOIN start_sec ss
    WHERE
      cs.sec BETWEEN ss.s0 AND ss.s1
  ),
  labeled AS (
    SELECT
      rel_sec,
      CASE
        WHEN cc.n = 8
        AND cpu <= 3 THEN 'Little (0–3)'
        WHEN cc.n = 8
        AND cpu <= 6 THEN 'Mid (4–6)'
        WHEN cc.n = 8 THEN 'Big (7)'
        WHEN cc.n > 4 THEN 'Cluster ' || (cpu / 4)
        ELSE 'CPU ' || cpu
      END AS cluster,
      mhz
    FROM per_cpu_sec, cpu_cnt cc
    WHERE
      rel_sec >= 0
  )
SELECT cast_double!(rel_sec) AS rel_s, cluster, round(avg(mhz), 0) AS mhz
FROM labeled
GROUP BY
  rel_sec,
  cluster
ORDER BY
  rel_sec,
  cluster;

-- Per-core CPU utilization time series (1s buckets) for Core x Time heatmaps.
-- Uses cpustat counters when available, falling back to _sysmon_sched_cpu_sec.
CREATE PERFETTO FUNCTION sysmon_cpu_core_series(start_ts LONG, end_ts LONG)
RETURNS TABLE(rel_s DOUBLE, cpu LONG, busy_pct DOUBLE)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  start_sec AS (
    SELECT w.start_ts / 1000000000 AS s0, w.end_ts / 1000000000 AS s1 FROM w
  ),
  cs_tracks AS MATERIALIZED (
    SELECT id, cpu, name FROM cpu_counter_track WHERE type = 'cpustat'
  ),
  cs_step AS (
    SELECT
      cs.sec - ss.s0 AS rel_sec,
      ct.cpu,
      ct.name,
      c.value - lag(c.value) OVER (PARTITION BY cs.track_id ORDER BY cs.sec) AS dv
    FROM cs_tracks ct
    JOIN _sysmon_counter_sec cs ON cs.track_id = ct.id
    JOIN counter c ON c.id = cs.last_id
    JOIN start_sec ss
    JOIN w
    WHERE
      w.has_cpustat = 1
      AND cs.sec BETWEEN ss.s0 AND ss.s1
  ),
  cs_agg AS (
    SELECT
      rel_sec,
      cpu,
      round(
        100.0
        * sum(
          iif(
            name NOT IN ('cpu.times.idle_ns', 'cpu.times.io_wait_ns'),
            dv,
            0.0
          )
        )
        / max(1.0, sum(dv)),
        1
      ) AS busy_pct
    FROM cs_step
    WHERE
      dv IS NOT NULL
      AND dv >= 0
      AND rel_sec >= 0
    GROUP BY
      rel_sec,
      cpu
  ),
  sc_agg AS (
    SELECT
      sc.sec - ss.s0 AS rel_sec,
      sc.cpu,
      round(min(100.0, 100.0 * sc.run_ns / 1e9), 1) AS busy_pct
    FROM _sysmon_sched_cpu_sec sc, start_sec ss, w
    WHERE
      w.has_cpustat = 0
      AND sc.sec BETWEEN ss.s0 AND ss.s1
      AND sc.sec >= ss.s0
  )
SELECT cast_double!(rel_sec) AS rel_s, cpu, busy_pct FROM cs_agg
UNION ALL
SELECT cast_double!(rel_sec) AS rel_s, cpu, busy_pct FROM sc_agg
ORDER BY
  rel_s,
  cpu;

-- Where active (non-idle) thread time went across the system (Snapshot / traces
-- with scheduling data). Uses _sysmon_thread_state_agg when unbrushed (~0 ms!).
CREATE PERFETTO FUNCTION sysmon_wait_breakdown(start_ts LONG, end_ts LONG)
RETURNS TABLE(
  category STRING,
  label STRING,
  total_ms DOUBLE,
  share_pct DOUBLE,
  event_count LONG
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  classified AS (
    SELECT
      CASE
        WHEN a.state = 'Running' THEN 'running'
        WHEN a.state IN ('R', 'R+') THEN 'runnable'
        WHEN a.state = 'D'
        AND a.io_wait = 1 THEN 'io_wait'
        WHEN a.state = 'D' THEN 'uninterruptible'
        ELSE NULL
      END AS category,
      a.total_ns AS clip_ns,
      a.cnt AS event_count
    FROM _sysmon_thread_state_agg a
    WHERE
      $start_ts IS NULL
      AND $end_ts IS NULL
    UNION ALL
    SELECT
      CASE
        WHEN ts.state = 'Running' THEN 'running'
        WHEN ts.state IN ('R', 'R+') THEN 'runnable'
        WHEN ts.state = 'D'
        AND ts.io_wait = 1 THEN 'io_wait'
        WHEN ts.state = 'D' THEN 'uninterruptible'
        ELSE NULL
      END AS category,
      max(0, min(ts.ts + ts.dur, w.end_ts) - max(ts.ts, w.start_ts)) AS clip_ns,
      1 AS event_count
    FROM thread_state ts, w
    WHERE
      ($start_ts IS NOT NULL OR $end_ts IS NOT NULL)
      AND ts.utid != 0
      AND ts.dur > 0
      AND ts.ts < w.end_ts
      AND ts.ts + ts.dur > w.start_ts
  ),
  grouped AS (
    SELECT
      category,
      sum(clip_ns) / 1e6 AS total_ms,
      sum(event_count) AS event_count
    FROM classified
    WHERE
      category IS NOT NULL
      AND clip_ns > 0
    GROUP BY
      category
  ),
  tot AS (SELECT max(1e-6, sum(total_ms)) AS s FROM grouped)
SELECT
  g.category,
  CASE g.category
    WHEN 'running' THEN 'Running on CPU'
    WHEN 'runnable' THEN 'Runnable (CPU contention)'
    WHEN 'io_wait' THEN 'Disk / I/O Wait (D-state)'
    WHEN 'uninterruptible' THEN 'Uninterruptible Wait (locks / reclaim)'
  END AS label,
  round(g.total_ms, 1) AS total_ms,
  round(100.0 * g.total_ms / tot.s, 1) AS share_pct,
  g.event_count
FROM grouped g, tot
ORDER BY
  g.total_ms DESC;

-- Top threads by active (Running + Runnable + D-state) time.
CREATE PERFETTO FUNCTION sysmon_top_cpu_threads(
  start_ts LONG,
  end_ts LONG,
  lim LONG
)
RETURNS TABLE(
  utid LONG,
  tid LONG,
  thread_name STRING,
  process_name STRING,
  running_ms DOUBLE,
  runnable_ms DOUBLE,
  io_wait_ms DOUBLE,
  d_other_ms DOUBLE,
  total_active_ms DOUBLE
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  agg AS (
    SELECT
      a.utid,
      sum(iif(a.state = 'Running', a.total_ns, 0)) / 1e6 AS running_ms,
      sum(iif(a.state IN ('R', 'R+'), a.total_ns, 0)) / 1e6 AS runnable_ms,
      sum(iif(a.state = 'D' AND a.io_wait = 1, a.total_ns, 0)) / 1e6 AS io_wait_ms,
      sum(iif(a.state = 'D' AND coalesce(a.io_wait, 0) = 0, a.total_ns, 0))
      / 1e6 AS d_other_ms
    FROM _sysmon_thread_state_agg a
    WHERE
      $start_ts IS NULL
      AND $end_ts IS NULL
      AND a.state IN ('Running', 'R', 'R+', 'D')
    GROUP BY
      a.utid
    UNION ALL
    SELECT
      ts.utid,
      sum(
        iif(
          ts.state = 'Running',
          max(0, min(ts.ts + ts.dur, w.end_ts) - max(ts.ts, w.start_ts)),
          0
        )
      )
      / 1e6 AS running_ms,
      sum(
        iif(
          ts.state IN ('R', 'R+'),
          max(0, min(ts.ts + ts.dur, w.end_ts) - max(ts.ts, w.start_ts)),
          0
        )
      )
      / 1e6 AS runnable_ms,
      sum(
        iif(
          ts.state = 'D'
          AND ts.io_wait = 1,
          max(0, min(ts.ts + ts.dur, w.end_ts) - max(ts.ts, w.start_ts)),
          0
        )
      )
      / 1e6 AS io_wait_ms,
      sum(
        iif(
          ts.state = 'D'
          AND coalesce(ts.io_wait, 0) = 0,
          max(0, min(ts.ts + ts.dur, w.end_ts) - max(ts.ts, w.start_ts)),
          0
        )
      )
      / 1e6 AS d_other_ms
    FROM thread_state ts, w
    WHERE
      ($start_ts IS NOT NULL OR $end_ts IS NOT NULL)
      AND ts.utid != 0
      AND ts.dur > 0
      AND ts.state IN ('Running', 'R', 'R+', 'D')
      AND ts.ts < w.end_ts
      AND ts.ts + ts.dur > w.start_ts
    GROUP BY
      ts.utid
  )
SELECT
  a.utid,
  t.tid,
  coalesce(t.name, 'tid:' || t.tid) AS thread_name,
  coalesce(p.name, coalesce(t.name, 'kernel')) AS process_name,
  round(a.running_ms, 1) AS running_ms,
  round(a.runnable_ms, 1) AS runnable_ms,
  round(a.io_wait_ms, 1) AS io_wait_ms,
  round(a.d_other_ms, 1) AS d_other_ms,
  round(a.running_ms + a.runnable_ms + a.io_wait_ms + a.d_other_ms, 1) AS total_active_ms
FROM agg a
JOIN thread t USING (utid)
LEFT JOIN process p ON t.upid = p.upid
ORDER BY
  total_active_ms DESC
LIMIT coalesce($lim, 15);

-- Wait Explainer: breaks down WHY threads were waiting for a given category
-- ('runnable', 'io_wait', 'uninterruptible') by blocked function, thread, and waker.
CREATE PERFETTO FUNCTION sysmon_wait_explainer(
  start_ts LONG,
  end_ts LONG,
  category STRING,
  lim LONG
)
RETURNS TABLE(
  thread_name STRING,
  process_name STRING,
  blocked_function STRING,
  top_waker STRING,
  wait_count LONG,
  total_ms DOUBLE,
  max_ms DOUBLE
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  filtered AS (
    SELECT
      ts.utid,
      coalesce(ts.blocked_function, '(unsymbolized / runnable)') AS blocked_function,
      ts.waker_utid,
      max(0, min(ts.ts + ts.dur, w.end_ts) - max(ts.ts, w.start_ts)) / 1e6 AS ms
    FROM thread_state ts
    JOIN w
    WHERE
      ts.utid != 0
      AND ts.dur > 0
      AND ts.ts < w.end_ts
      AND ts.ts + ts.dur > w.start_ts
      AND (($category = 'runnable' AND ts.state IN ('R', 'R+'))
      OR ($category = 'io_wait' AND ts.state = 'D' AND ts.io_wait = 1)
      OR ($category = 'uninterruptible'
      AND ts.state = 'D'
      AND coalesce(ts.io_wait, 0) = 0))
  )
SELECT
  coalesce(t.name, 'tid:' || t.tid) AS thread_name,
  coalesce(p.name, coalesce(t.name, 'kernel')) AS process_name,
  f.blocked_function,
  coalesce(wt.name, 'unknown') AS top_waker,
  count(*) AS wait_count,
  round(sum(f.ms), 1) AS total_ms,
  round(max(f.ms), 1) AS max_ms
FROM filtered f
JOIN thread t ON f.utid = t.utid
LEFT JOIN process p ON t.upid = p.upid
LEFT JOIN thread wt ON f.waker_utid = wt.utid
WHERE
  f.ms > 0
GROUP BY
  f.utid,
  f.blocked_function,
  wt.name
ORDER BY
  total_ms DESC
LIMIT coalesce($lim, 15);
