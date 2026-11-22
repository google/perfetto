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

INCLUDE PERFETTO MODULE sysmon.apps;

-- Unified cross-resource process table (Task Manager + Snapshot Bottlenecks).
-- Uses precomputed pipeline summaries so it runs in a few milliseconds even
-- when returning 150+ processes for Live Task Manager filtering.
CREATE PERFETTO FUNCTION sysmon_top_processes(
  start_ts LONG,
  end_ts LONG,
  lim LONG
)
RETURNS TABLE(
  upid LONG,
  pid LONG,
  process_name STRING,
  cpu_ms DOUBLE,
  cpu_pct DOUBLE,
  runnable_ms DOUBLE,
  anon_swap_mib DOUBLE,
  mem_delta_mib DOUBLE,
  io_mb DOUBLE,
  io_wait_ms DOUBLE,
  oom_score_adj LONG,
  dominant_resource STRING,
  anon_mib DOUBLE,
  swap_mib DOUBLE,
  file_mib DOUBLE,
  dmabuf_mib DOUBLE,
  age_s DOUBLE,
  is_top LONG,
  frozen LONG
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  wt AS MATERIALIZED (SELECT * FROM _sysmon_window_tracks($start_ts, $end_ts)),
  top_info AS MATERIALIZED (
    SELECT top_app, top_upid FROM sysmon_top_app($start_ts, $end_ts)
  ),
  cpu_sched AS MATERIALIZED (
    SELECT
      t.upid,
      sum(iif(a.state = 'Running', a.total_ns, 0)) / 1e6 AS cpu_ms,
      sum(iif(a.state IN ('R', 'R+'), a.total_ns, 0)) / 1e6 AS runnable_ms,
      sum(iif(a.state = 'D' AND a.io_wait = 1, a.total_ns, 0)) / 1e6 AS io_wait_ms
    FROM _sysmon_thread_state_agg a
    JOIN thread t ON a.utid = t.utid
    WHERE
      $start_ts IS NULL
      AND $end_ts IS NULL
      AND t.upid IS NOT NULL
      AND a.state IN ('Running', 'R', 'R+', 'D')
    GROUP BY
      t.upid
    UNION ALL
    SELECT
      t.upid,
      sum(
        iif(
          ts.state = 'Running',
          max(0, min(ts.ts + ts.dur, w.end_ts) - max(ts.ts, w.start_ts)),
          0
        )
      )
      / 1e6 AS cpu_ms,
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
      / 1e6 AS io_wait_ms
    FROM thread_state ts
    JOIN w
    JOIN thread t ON ts.utid = t.utid
    WHERE
      ($start_ts IS NOT NULL OR $end_ts IS NOT NULL)
      AND t.upid IS NOT NULL
      AND ts.dur > 0
      AND ts.state IN ('Running', 'R', 'R+', 'D')
      AND ts.ts < w.end_ts
      AND ts.ts + ts.dur > w.start_ts
    GROUP BY
      t.upid
  ),
  proc_ct AS MATERIALIZED (
    SELECT
      t.upid,
      sum(iif(t.type = 'proc_stat_runtime', max(0.0, wt.delta_v), 0.0)) / 1e6 AS cpu_ms,
      coalesce(max(iif(t.name = 'mem.rss.anon', wt.last_v, NULL)), 0.0)
      / 1048576.0 AS anon_mib,
      coalesce(max(iif(t.name = 'mem.rss.anon', wt.first_v, NULL)), 0.0)
      / 1048576.0 AS anon_first_mib,
      coalesce(max(iif(t.name = 'mem.swap', wt.last_v, NULL)), 0.0) / 1048576.0 AS swap_mib,
      coalesce(max(iif(t.name = 'mem.swap', wt.first_v, NULL)), 0.0) / 1048576.0 AS swap_first_mib,
      coalesce(max(iif(t.name = 'mem.rss.file', wt.last_v, NULL)), 0.0)
      / 1048576.0 AS file_mib,
      coalesce(max(iif(t.name = 'mem.dmabuf_rss', wt.last_v, NULL)), 0.0)
      / 1048576.0 AS dmabuf_mib,
      cast_int!(max(iif(t.name = 'oom_score_adj', wt.last_v, NULL))) AS oom_score_adj
    FROM process_counter_track t
    JOIN wt ON wt.track_id = t.id
    WHERE
      t.type = 'proc_stat_runtime'
      OR t.name IN (
        'mem.rss.anon',
        'mem.swap',
        'mem.rss.file',
        'mem.dmabuf_rss',
        'oom_score_adj'
      )
    GROUP BY
      t.upid
  ),
  frozen_upids AS MATERIALIZED (
    SELECT DISTINCT t.upid
    FROM ftrace_event fe
    JOIN thread t ON t.utid = fe.utid
    JOIN w
    WHERE
      EXISTS (
        SELECT 1
        FROM _sysmon_ftrace_counts
        WHERE
          name = 'cgroup_notify_frozen'
        LIMIT 1
      )
      AND fe.name = 'cgroup_notify_frozen'
      AND fe.ts BETWEEN w.start_ts AND w.end_ts
      AND coalesce(extract_arg(fe.arg_set_id, 'val'), 1) != 0
      AND t.upid IS NOT NULL
  ),
  sto_blk AS MATERIALIZED (
    SELECT
      t.upid,
      sum(
        coalesce(extract_arg(fe.arg_set_id, 'nr_sector'), 0) * 512.0 / 1048576.0
      ) AS total_io_mb
    FROM ftrace_event fe
    JOIN w
    JOIN thread t ON fe.utid = t.utid
    WHERE
      w.has_block = 1
      AND t.upid IS NOT NULL
      AND fe.name = 'block_rq_issue'
      AND fe.ts BETWEEN w.start_ts AND w.end_ts
    GROUP BY
      t.upid
  ),
  upids AS (
    SELECT upid FROM cpu_sched
    UNION
    SELECT upid FROM proc_ct
    UNION
    SELECT upid FROM sto_blk
  ),
  joined AS (
    SELECT
      u.upid,
      p.pid,
      coalesce(p.name, 'pid:' || p.pid) AS process_name,
      coalesce(cs.cpu_ms, pc.cpu_ms, 0.0) AS cpu_ms,
      100.0 * coalesce(cs.cpu_ms, pc.cpu_ms, 0.0)
      / max(1.0, w.dur_s * 1000.0 * w.cpu_count) AS cpu_pct,
      coalesce(cs.runnable_ms, 0.0) AS runnable_ms,
      coalesce(pc.anon_mib, 0.0) AS anon_mib,
      coalesce(pc.swap_mib, 0.0) AS swap_mib,
      coalesce(pc.file_mib, 0.0) AS file_mib,
      coalesce(pc.dmabuf_mib, 0.0) AS dmabuf_mib,
      coalesce(pc.anon_mib + pc.swap_mib, 0.0) AS anon_swap_mib,
      coalesce(
        (pc.anon_mib + pc.swap_mib) - (pc.anon_first_mib + pc.swap_first_mib),
        0.0
      ) AS mem_delta_mib,
      coalesce(sb.total_io_mb, 0.0) AS io_mb,
      coalesce(cs.io_wait_ms, 0.0) AS io_wait_ms,
      pc.oom_score_adj,
      iif(
        p.start_ts IS NOT NULL
        AND p.start_ts > 0
        AND w.end_ts >= p.start_ts,
        round((w.end_ts - p.start_ts) / 1e9, 0),
        NULL
      ) AS age_s,
      iif(
        u.upid = (SELECT top_upid FROM top_info)
        OR p.name = (SELECT top_app FROM top_info),
        1,
        0
      ) AS is_top,
      iif(fu.upid IS NOT NULL, 1, 0) AS frozen
    FROM upids u
    JOIN process p USING (upid)
    JOIN w
    LEFT JOIN cpu_sched cs ON cs.upid = u.upid
    LEFT JOIN proc_ct pc ON pc.upid = u.upid
    LEFT JOIN sto_blk sb ON sb.upid = u.upid
    LEFT JOIN frozen_upids fu ON fu.upid = u.upid
  )
SELECT
  upid,
  pid,
  process_name,
  round(cpu_ms, 1) AS cpu_ms,
  round(cpu_pct, 1) AS cpu_pct,
  round(runnable_ms, 1) AS runnable_ms,
  round(anon_swap_mib, 1) AS anon_swap_mib,
  round(mem_delta_mib, 1) AS mem_delta_mib,
  round(io_mb, 2) AS io_mb,
  round(io_wait_ms, 1) AS io_wait_ms,
  oom_score_adj,
  CASE
    WHEN io_mb >= 10
    OR io_wait_ms >= 200 THEN 'Storage'
    WHEN abs(mem_delta_mib) >= 32
    OR anon_swap_mib >= 256 THEN 'Memory'
    WHEN cpu_pct >= 5
    OR runnable_ms >= 100 THEN 'CPU'
    WHEN anon_swap_mib > 0 THEN 'Memory'
    ELSE 'CPU'
  END AS dominant_resource,
  round(anon_mib, 1) AS anon_mib,
  round(swap_mib, 1) AS swap_mib,
  round(file_mib, 1) AS file_mib,
  round(dmabuf_mib, 1) AS dmabuf_mib,
  age_s,
  is_top,
  frozen
FROM joined
ORDER BY
  is_top DESC,
  (cpu_pct * 4.0 + anon_swap_mib / 40.0 + abs(mem_delta_mib) * 1.5 + io_mb * 2.0
  + io_wait_ms / 50.0) DESC
LIMIT coalesce($lim, 15);

-- Compact binned per-process CPU% and Anon+Swap MiB trend arrays for inline
-- SVG sparklines in the Task Manager and Apps tables. Runs in a single pass
-- over _sysmon_counter_sec for the top/privileged processes.
CREATE PERFETTO FUNCTION sysmon_process_trends(
  start_ts LONG,
  end_ts LONG,
  lim_procs LONG,
  num_bins LONG
)
RETURNS TABLE(upid LONG, bin LONG, cpu_pct DOUBLE, anon_swap_mib DOUBLE)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  nb AS (SELECT max(4, min(24, coalesce($num_bins, 15))) AS n),
  s_bounds AS (
    SELECT
      w.start_ts / 1000000000 AS s0,
      w.end_ts / 1000000000 AS s1,
      max(1.0, (w.end_ts - w.start_ts) / 1e9) AS dur_s
    FROM w
  ),
  picked AS MATERIALIZED (
    SELECT upid
    FROM sysmon_top_processes($start_ts, $end_ts, coalesce($lim_procs, 80))
  ),
  p_tracks AS MATERIALIZED (
    SELECT t.id, t.upid, t.type, t.name
    FROM process_counter_track t
    JOIN picked pk ON pk.upid = t.upid
    WHERE
      t.type = 'proc_stat_runtime'
      OR t.name IN ('mem.rss.anon', 'mem.swap')
  ),
  mem_binned AS (
    SELECT
      pt.upid,
      min(
        nb.n - 1,
        max(
          0,
          cast_int!(
            ((cs.max_ts - w.start_ts) * nb.n) / max(1, w.end_ts - w.start_ts)
          )
        )
      ) AS bin,
      max(iif(pt.name = 'mem.rss.anon', c.value, 0.0)) AS anon_b,
      max(iif(pt.name = 'mem.swap', c.value, 0.0)) AS swap_b
    FROM p_tracks pt
    JOIN _sysmon_counter_sec cs ON cs.track_id = pt.id
    JOIN counter c ON c.id = cs.last_id
    JOIN s_bounds sb, w, nb
    WHERE
      pt.name IN ('mem.rss.anon', 'mem.swap')
      AND cs.sec BETWEEN sb.s0 AND sb.s1
    GROUP BY
      pt.upid,
      2
  ),
  cpu_raw AS (
    SELECT
      pt.upid,
      cs.track_id,
      cs.max_ts,
      c.value - lag(c.value) OVER (PARTITION BY cs.track_id ORDER BY cs.sec) AS dv,
      cs.max_ts - lag(cs.max_ts) OVER (PARTITION BY cs.track_id ORDER BY cs.sec) AS dt
    FROM p_tracks pt
    JOIN _sysmon_counter_sec cs ON cs.track_id = pt.id
    JOIN counter c ON c.id = cs.last_id
    JOIN s_bounds sb
    WHERE
      pt.type = 'proc_stat_runtime'
      AND cs.sec BETWEEN sb.s0 AND sb.s1
  ),
  has_proc_rt AS (
    SELECT
      iif(
        EXISTS (SELECT 1 FROM p_tracks WHERE type = 'proc_stat_runtime' LIMIT 1),
        1,
        0
      ) AS flag
  ),
  cpu_binned AS (
    SELECT
      cr.upid,
      min(
        nb.n - 1,
        max(
          0,
          cast_int!(
            ((cr.max_ts - w.start_ts) * nb.n) / max(1, w.end_ts - w.start_ts)
          )
        )
      ) AS bin,
      100.0 * sum(max(0.0, cr.dv)) / max(1.0, sum(cr.dt) * w.cpu_count) AS cpu_pct
    FROM cpu_raw cr, w, nb
    WHERE
      cr.dv IS NOT NULL
      AND cr.dt IS NOT NULL
      AND cr.dt > 0
    GROUP BY
      cr.upid,
      2
    UNION ALL
    SELECT
      t.upid,
      min(
        nb.n - 1,
        max(
          0,
          cast_int!(
            ((s.ts - w.start_ts) * nb.n) / max(1, w.end_ts - w.start_ts)
          )
        )
      ) AS bin,
      100.0 * sum(s.dur)
      / max(1.0, ((w.end_ts - w.start_ts) / nb.n) * w.cpu_count) AS cpu_pct
    FROM picked pk
    JOIN thread t ON t.upid = pk.upid
    JOIN sched s ON s.utid = t.utid
    JOIN w, nb, has_proc_rt h
    WHERE
      h.flag = 0
      AND w.has_sched = 1
      AND s.dur > 0
      AND s.ts BETWEEN w.start_ts AND w.end_ts
    GROUP BY
      t.upid,
      2
  ),
  keys AS (
    SELECT upid, bin FROM mem_binned
    UNION
    SELECT upid, bin FROM cpu_binned
  )
SELECT
  k.upid,
  k.bin,
  round(coalesce(cb.cpu_pct, 0.0), 1) AS cpu_pct,
  round((coalesce(mb.anon_b, 0.0) + coalesce(mb.swap_b, 0.0)) / 1048576.0, 1) AS anon_swap_mib
FROM keys k
LEFT JOIN mem_binned mb ON mb.upid = k.upid AND mb.bin = k.bin
LEFT JOIN cpu_binned cb ON cb.upid = k.upid AND cb.bin = k.bin
ORDER BY
  k.upid,
  k.bin;

-- Detailed time series for a single process tab (Live & Snapshot):
-- CPU%, Anon, Swap, File, Shmem, DMA-BUF, and oom_score_adj over time.
CREATE PERFETTO FUNCTION sysmon_process_detail_series(
  target_upid LONG,
  start_ts LONG,
  end_ts LONG
)
RETURNS TABLE(
  rel_s DOUBLE,
  cpu_pct DOUBLE,
  anon_mib DOUBLE,
  swap_mib DOUBLE,
  file_mib DOUBLE,
  shmem_mib DOUBLE,
  dmabuf_mib DOUBLE,
  oom_score_adj LONG
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  s_bounds AS (
    SELECT w.start_ts / 1000000000 AS s0, w.end_ts / 1000000000 AS s1 FROM w
  ),
  p_tracks AS MATERIALIZED (
    SELECT id, type, name
    FROM process_counter_track
    WHERE
      upid = $target_upid
      AND (type = 'proc_stat_runtime'
      OR name IN (
        'mem.rss.anon',
        'mem.swap',
        'mem.rss.file',
        'mem.rss.shmem',
        'mem.dmabuf_rss',
        'oom_score_adj'
      ))
  ),
  mem_sec AS (
    SELECT
      cs.sec - sb.s0 AS rel_sec,
      max(
        iif(
          pt.name IN (
            'mem.rss.anon',
            'mem.swap',
            'mem.rss.file',
            'mem.rss.shmem',
            'mem.dmabuf_rss'
          ),
          1,
          0
        )
      ) AS has_mem,
      max(iif(pt.name = 'mem.rss.anon', c.value, NULL)) / 1048576.0 AS anon_mib,
      max(iif(pt.name = 'mem.swap', c.value, NULL)) / 1048576.0 AS swap_mib,
      max(iif(pt.name = 'mem.rss.file', c.value, NULL)) / 1048576.0 AS file_mib,
      max(iif(pt.name = 'mem.rss.shmem', c.value, NULL)) / 1048576.0 AS shmem_mib,
      max(iif(pt.name = 'mem.dmabuf_rss', c.value, NULL)) / 1048576.0 AS dmabuf_mib,
      cast_int!(max(iif(pt.name = 'oom_score_adj', c.value, NULL))) AS oom_score_adj
    FROM p_tracks pt
    JOIN _sysmon_counter_sec cs ON cs.track_id = pt.id
    JOIN counter c ON c.id = cs.last_id
    JOIN s_bounds sb
    WHERE
      pt.type != 'proc_stat_runtime'
      AND cs.sec BETWEEN sb.s0 AND sb.s1
    GROUP BY
      cs.sec
  ),
  cpu_step AS (
    SELECT
      cs.sec - sb.s0 AS rel_sec,
      c.value - lag(c.value) OVER (PARTITION BY cs.track_id ORDER BY cs.sec) AS dv,
      cs.max_ts - lag(cs.max_ts) OVER (PARTITION BY cs.track_id ORDER BY cs.sec) AS dt
    FROM p_tracks pt
    JOIN _sysmon_counter_sec cs ON cs.track_id = pt.id
    JOIN counter c ON c.id = cs.last_id
    JOIN s_bounds sb
    WHERE
      pt.type = 'proc_stat_runtime'
      AND cs.sec BETWEEN sb.s0 AND sb.s1
  ),
  cpu_sec AS (
    SELECT
      rel_sec,
      100.0 * sum(max(0.0, dv)) / max(1.0, sum(dt) * w.cpu_count) AS cpu_pct
    FROM cpu_step, w
    WHERE
      dv IS NOT NULL
      AND dt IS NOT NULL
      AND dt > 0
    GROUP BY
      rel_sec
  ),
  sched_sec AS (
    SELECT
      cast_int!(s.ts / 1000000000) - sb.s0 AS rel_sec,
      100.0 * sum(s.dur) / (1e9 * w.cpu_count) AS cpu_pct
    FROM sched s
    JOIN thread t ON t.utid = s.utid
    JOIN w, s_bounds sb
    WHERE
      w.has_sched = 1
      AND t.upid = $target_upid
      AND s.dur > 0
      AND s.ts BETWEEN w.start_ts AND w.end_ts
    GROUP BY
      1
  ),
  all_secs AS (
    SELECT rel_sec FROM mem_sec
    UNION
    SELECT rel_sec FROM cpu_sec
    UNION
    SELECT rel_sec FROM sched_sec
  )
SELECT
  cast_double!(a.rel_sec) AS rel_s,
  round(coalesce(ss.cpu_pct, cs.cpu_pct, 0.0), 1) AS cpu_pct,
  round(ms.anon_mib, 1) AS anon_mib,
  round(ms.swap_mib, 1) AS swap_mib,
  round(ms.file_mib, 1) AS file_mib,
  round(ms.shmem_mib, 1) AS shmem_mib,
  round(ms.dmabuf_mib, 1) AS dmabuf_mib,
  ms.oom_score_adj
FROM all_secs a
LEFT JOIN mem_sec ms USING (rel_sec)
LEFT JOIN cpu_sec cs USING (rel_sec)
LEFT JOIN sched_sec ss USING (rel_sec)
WHERE
  a.rel_sec >= 0
ORDER BY
  a.rel_sec;

-- Per-thread active time breakdown for a single process in Snapshot mode,
-- including CPU delay ratio (runnable / running) and top blocked function.
CREATE PERFETTO FUNCTION sysmon_process_threads(
  target_upid LONG,
  start_ts LONG,
  end_ts LONG,
  lim LONG
)
RETURNS TABLE(
  utid LONG,
  tid LONG,
  thread_name STRING,
  running_ms DOUBLE,
  runnable_ms DOUBLE,
  io_wait_ms DOUBLE,
  d_other_ms DOUBLE,
  total_active_ms DOUBLE,
  cpu_delay_ratio DOUBLE,
  top_blocked_fn STRING
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  p_threads AS MATERIALIZED (
    SELECT utid, tid, coalesce(name, 'tid:' || tid) AS thread_name
    FROM thread
    WHERE
      upid = $target_upid
  ),
  agg AS (
    SELECT
      pt.utid,
      sum(iif(a.state = 'Running', a.total_ns, 0)) / 1e6 AS running_ms,
      sum(iif(a.state IN ('R', 'R+'), a.total_ns, 0)) / 1e6 AS runnable_ms,
      sum(iif(a.state = 'D' AND a.io_wait = 1, a.total_ns, 0)) / 1e6 AS io_wait_ms,
      sum(iif(a.state = 'D' AND coalesce(a.io_wait, 0) = 0, a.total_ns, 0))
      / 1e6 AS d_other_ms
    FROM p_threads pt
    JOIN _sysmon_thread_state_agg a ON a.utid = pt.utid
    WHERE
      $start_ts IS NULL
      AND $end_ts IS NULL
      AND a.state IN ('Running', 'R', 'R+', 'D')
    GROUP BY
      pt.utid
    UNION ALL
    SELECT
      pt.utid,
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
    FROM p_threads pt
    JOIN thread_state ts ON ts.utid = pt.utid
    JOIN w
    WHERE
      ($start_ts IS NOT NULL OR $end_ts IS NOT NULL)
      AND ts.dur > 0
      AND ts.state IN ('Running', 'R', 'R+', 'D')
      AND ts.ts < w.end_ts
      AND ts.ts + ts.dur > w.start_ts
    GROUP BY
      pt.utid
  ),
  top_blk AS (
    SELECT utid, blocked_function
    FROM (
      SELECT
        pt.utid,
        ts.blocked_function,
        row_number() OVER (PARTITION BY pt.utid ORDER BY sum(ts.dur) DESC) AS rn
      FROM p_threads pt
      JOIN thread_state ts ON ts.utid = pt.utid
      JOIN w
      WHERE
        ts.state = 'D'
        AND ts.blocked_function IS NOT NULL
        AND ts.dur > 0
        AND ts.ts < w.end_ts
        AND ts.ts + ts.dur > w.start_ts
      GROUP BY
        pt.utid,
        ts.blocked_function
    )
    WHERE
      rn = 1
  )
SELECT
  a.utid,
  pt.tid,
  pt.thread_name,
  round(a.running_ms, 1) AS running_ms,
  round(a.runnable_ms, 1) AS runnable_ms,
  round(a.io_wait_ms, 1) AS io_wait_ms,
  round(a.d_other_ms, 1) AS d_other_ms,
  round(a.running_ms + a.runnable_ms + a.io_wait_ms + a.d_other_ms, 1) AS total_active_ms,
  round(iif(a.running_ms >= 0.5, a.runnable_ms / a.running_ms, 0.0), 2) AS cpu_delay_ratio,
  tb.blocked_function AS top_blocked_fn
FROM agg a
JOIN p_threads pt ON pt.utid = a.utid
LEFT JOIN top_blk tb ON tb.utid = a.utid
ORDER BY
  total_active_ms DESC
LIMIT coalesce($lim, 25);

-- Runnable / wakeup scheduling delay histogram for a single process in Snapshot.
CREATE PERFETTO FUNCTION sysmon_process_runnable_hist(
  target_upid LONG,
  start_ts LONG,
  end_ts LONG
)
RETURNS TABLE(bucket_order LONG, bucket_label STRING, cnt LONG)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  p_threads AS MATERIALIZED (SELECT utid FROM thread WHERE upid = $target_upid),
  b AS (
    SELECT
      CASE
        WHEN ts.dur < 100000 THEN 0
        WHEN ts.dur < 500000 THEN 1
        WHEN ts.dur < 1000000 THEN 2
        WHEN ts.dur < 2000000 THEN 3
        WHEN ts.dur < 5000000 THEN 4
        WHEN ts.dur < 10000000 THEN 5
        WHEN ts.dur < 25000000 THEN 6
        ELSE 7
      END AS ord
    FROM p_threads pt
    JOIN thread_state ts ON ts.utid = pt.utid
    JOIN w
    WHERE
      ts.state IN ('R', 'R+')
      AND ts.dur > 0
      AND ts.ts < w.end_ts
      AND ts.ts + ts.dur > w.start_ts
  )
SELECT
  ord AS bucket_order,
  CASE ord
    WHEN 0 THEN '<0.1ms'
    WHEN 1 THEN '0.1–0.5ms'
    WHEN 2 THEN '0.5–1ms'
    WHEN 3 THEN '1–2ms'
    WHEN 4 THEN '2–5ms'
    WHEN 5 THEN '5–10ms'
    WHEN 6 THEN '10–25ms'
    ELSE '>25ms'
  END AS bucket_label,
  count(*) AS cnt
FROM b
GROUP BY
  ord
ORDER BY
  ord;
