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

-- Physical RAM (MemTotal) ledger at the end of the window plus start-to-end
-- change, so every gigabyte of physical RAM is accounted for.
CREATE PERFETTO FUNCTION sysmon_memtotal_ledger(start_ts LONG, end_ts LONG)
RETURNS TABLE(
  category STRING,
  bytes DOUBLE,
  delta_bytes DOUBLE,
  pct_of_total DOUBLE
)
AS
WITH
  wt AS MATERIALIZED (SELECT * FROM _sysmon_window_tracks($start_ts, $end_ts)),
  agg AS (
    SELECT
      max(iif(ct.name = 'MemTotal', wt.last_v, NULL)) AS mem_total,
      coalesce(max(iif(ct.name = 'AnonPages', wt.last_v, NULL)), 0) AS anon_last,
      coalesce(max(iif(ct.name = 'AnonPages', wt.first_v, NULL)), 0) AS anon_first,
      coalesce(max(iif(ct.name = 'Cached', wt.last_v, NULL)), 0)
      + coalesce(max(iif(ct.name = 'Buffers', wt.last_v, NULL)), 0) AS cache_last,
      coalesce(max(iif(ct.name = 'Cached', wt.first_v, NULL)), 0)
      + coalesce(max(iif(ct.name = 'Buffers', wt.first_v, NULL)), 0) AS cache_first,
      coalesce(max(iif(ct.name = 'Slab', wt.last_v, NULL)), 0)
      + coalesce(max(iif(ct.name = 'KernelStack', wt.last_v, NULL)), 0)
      + coalesce(max(iif(ct.name = 'PageTables', wt.last_v, NULL)), 0)
      + coalesce(max(iif(ct.name = 'VmallocUsed', wt.last_v, NULL)), 0) AS kern_last,
      coalesce(max(iif(ct.name = 'Slab', wt.first_v, NULL)), 0)
      + coalesce(max(iif(ct.name = 'KernelStack', wt.first_v, NULL)), 0)
      + coalesce(max(iif(ct.name = 'PageTables', wt.first_v, NULL)), 0)
      + coalesce(max(iif(ct.name = 'VmallocUsed', wt.first_v, NULL)), 0) AS kern_first,
      coalesce(max(iif(ct.name = 'ION_heap', wt.last_v, NULL)), 0)
      + coalesce(max(iif(ct.name = 'ION_heap_pool', wt.last_v, NULL)), 0)
      + coalesce(max(iif(ct.name = 'Gpu', wt.last_v, NULL)), 0) AS dma_last,
      coalesce(max(iif(ct.name = 'ION_heap', wt.first_v, NULL)), 0)
      + coalesce(max(iif(ct.name = 'ION_heap_pool', wt.first_v, NULL)), 0)
      + coalesce(max(iif(ct.name = 'Gpu', wt.first_v, NULL)), 0) AS dma_first,
      coalesce(max(iif(ct.name = 'Zram', wt.last_v, NULL)), 0) AS zram_last,
      coalesce(max(iif(ct.name = 'Zram', wt.first_v, NULL)), 0) AS zram_first,
      coalesce(max(iif(ct.name = 'MemFree', wt.last_v, NULL)), 0) AS free_last,
      coalesce(max(iif(ct.name = 'MemFree', wt.first_v, NULL)), 0) AS free_first
    FROM counter_track ct
    JOIN wt ON wt.track_id = ct.id
    WHERE
      ct.type = 'meminfo'
  ),
  rows AS (
    SELECT
      'Anon (Processes)' AS category,
      anon_last AS bytes,
      anon_last - anon_first AS delta_bytes,
      mem_total
    FROM agg
    UNION ALL
    SELECT 'File Cache', cache_last, cache_last - cache_first, mem_total
    FROM agg
    UNION ALL
    SELECT 'Kernel & Slab', kern_last, kern_last - kern_first, mem_total
    FROM agg
    UNION ALL
    SELECT 'DMA-BUF & GPU', dma_last, dma_last - dma_first, mem_total FROM agg
    UNION ALL
    SELECT 'ZRAM', zram_last, zram_last - zram_first, mem_total FROM agg
    UNION ALL
    SELECT 'Free', free_last, free_last - free_first, mem_total FROM agg
    UNION ALL
    SELECT
      'Other / Reserved',
      max(
        0.0,
        coalesce(mem_total, 0)
        - (anon_last + cache_last + kern_last + dma_last + zram_last + free_last)
      ),
      0.0,
      mem_total
    FROM agg
  )
SELECT
  category,
  bytes,
  delta_bytes,
  round(100.0 * bytes / max(1.0, mem_total), 1) AS pct_of_total
FROM rows
WHERE
  mem_total IS NOT NULL
  AND mem_total > 0;

-- Full physical RAM partition time series (MiB) at 1s resolution from pipeline
-- counter buckets, so stacked area charts sum cleanly to MemTotal.
CREATE PERFETTO FUNCTION sysmon_memory_series(start_ts LONG, end_ts LONG)
RETURNS TABLE(
  rel_s DOUBLE,
  anon_mib DOUBLE,
  cache_mib DOUBLE,
  kernel_slab_mib DOUBLE,
  dmabuf_gpu_mib DOUBLE,
  zram_mib DOUBLE,
  free_mib DOUBLE,
  other_mib DOUBLE,
  avail_mib DOUBLE
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  start_sec AS (
    SELECT w.start_ts / 1000000000 AS s0, w.end_ts / 1000000000 AS s1 FROM w
  ),
  mi_tracks AS MATERIALIZED (
    SELECT id, name
    FROM counter_track
    WHERE
      type = 'meminfo'
      AND name IN (
        'MemTotal',
        'MemFree',
        'MemAvailable',
        'AnonPages',
        'Cached',
        'Buffers',
        'Slab',
        'KernelStack',
        'PageTables',
        'VmallocUsed',
        'ION_heap',
        'ION_heap_pool',
        'Gpu',
        'Zram'
      )
  ),
  per_sec AS (
    SELECT
      cs.sec - ss.s0 AS rel_sec,
      coalesce(max(iif(mt.name = 'MemTotal', c.value, NULL)), 0.0) / 1048576.0 AS total_mib,
      coalesce(max(iif(mt.name = 'AnonPages', c.value, NULL)), 0.0) / 1048576.0 AS anon_mib,
      (coalesce(max(iif(mt.name = 'Cached', c.value, NULL)), 0.0)
      + coalesce(max(iif(mt.name = 'Buffers', c.value, NULL)), 0.0))
      / 1048576.0 AS cache_mib,
      (coalesce(max(iif(mt.name = 'Slab', c.value, NULL)), 0.0)
      + coalesce(max(iif(mt.name = 'KernelStack', c.value, NULL)), 0.0)
      + coalesce(max(iif(mt.name = 'PageTables', c.value, NULL)), 0.0)
      + coalesce(max(iif(mt.name = 'VmallocUsed', c.value, NULL)), 0.0))
      / 1048576.0 AS kernel_slab_mib,
      (coalesce(max(iif(mt.name = 'ION_heap', c.value, NULL)), 0.0)
      + coalesce(max(iif(mt.name = 'ION_heap_pool', c.value, NULL)), 0.0)
      + coalesce(max(iif(mt.name = 'Gpu', c.value, NULL)), 0.0))
      / 1048576.0 AS dmabuf_gpu_mib,
      coalesce(max(iif(mt.name = 'Zram', c.value, NULL)), 0.0) / 1048576.0 AS zram_mib,
      coalesce(max(iif(mt.name = 'MemFree', c.value, NULL)), 0.0) / 1048576.0 AS free_mib,
      coalesce(max(iif(mt.name = 'MemAvailable', c.value, NULL)), 0.0)
      / 1048576.0 AS avail_mib
    FROM mi_tracks mt
    JOIN _sysmon_counter_sec cs ON cs.track_id = mt.id
    JOIN counter c ON c.id = cs.last_id
    JOIN start_sec ss
    WHERE
      cs.sec BETWEEN ss.s0 AND ss.s1
    GROUP BY
      cs.sec
  )
SELECT
  cast_double!(rel_sec) AS rel_s,
  round(anon_mib, 1) AS anon_mib,
  round(cache_mib, 1) AS cache_mib,
  round(kernel_slab_mib, 1) AS kernel_slab_mib,
  round(dmabuf_gpu_mib, 1) AS dmabuf_gpu_mib,
  round(zram_mib, 1) AS zram_mib,
  round(free_mib, 1) AS free_mib,
  round(
    max(
      0.0,
      total_mib
      - (anon_mib + cache_mib + kernel_slab_mib + dmabuf_gpu_mib + zram_mib
      + free_mib)
    ),
    1
  ) AS other_mib,
  round(avail_mib, 1) AS avail_mib
FROM per_sec
WHERE
  rel_sec >= 0
  AND total_mib > 0
ORDER BY
  rel_sec;

-- Memory PSI (%) and vmstat paging/reclaim rate time series at 1s resolution.
CREATE PERFETTO FUNCTION sysmon_memory_pressure_series(
  start_ts LONG,
  end_ts LONG
)
RETURNS TABLE(
  rel_s DOUBLE,
  psi_mem_some_pct DOUBLE,
  psi_mem_full_pct DOUBLE,
  pgmajfault_s DOUBLE,
  pswpin_s DOUBLE,
  pswpout_s DOUBLE,
  reclaim_scan_s DOUBLE
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  start_sec AS (
    SELECT w.start_ts / 1000000000 AS s0, w.end_ts / 1000000000 AS s1 FROM w
  ),
  tracks AS MATERIALIZED (
    SELECT id, type, name
    FROM counter_track
    WHERE
      (type = 'psi' AND name IN ('psi.mem.some', 'psi.mem.full'))
      OR (type = 'vmstat'
      AND name IN (
        'pgmajfault',
        'pswpin',
        'pswpout',
        'pgscan_direct',
        'pgscan_kswapd'
      ))
  ),
  step AS (
    SELECT
      cs.sec - ss.s0 AS rel_sec,
      t.type,
      t.name,
      max(
        0.0,
        c.value - lag(c.value) OVER (PARTITION BY cs.track_id ORDER BY cs.sec)
      ) AS dv,
      (cs.max_ts
      - lag(cs.max_ts) OVER (PARTITION BY cs.track_id ORDER BY cs.sec)) AS dt_ns
    FROM tracks t
    JOIN _sysmon_counter_sec cs ON cs.track_id = t.id
    JOIN counter c ON c.id = cs.last_id
    JOIN start_sec ss
    WHERE
      cs.sec BETWEEN ss.s0 AND ss.s1
  )
SELECT
  cast_double!(rel_sec) AS rel_s,
  round(
    coalesce(
      max(iif(name = 'psi.mem.some' AND dt_ns > 0, 100.0 * dv / dt_ns, NULL)),
      0.0
    ),
    2
  ) AS psi_mem_some_pct,
  round(
    coalesce(
      max(iif(name = 'psi.mem.full' AND dt_ns > 0, 100.0 * dv / dt_ns, NULL)),
      0.0
    ),
    2
  ) AS psi_mem_full_pct,
  round(
    coalesce(
      sum(iif(name = 'pgmajfault' AND dt_ns > 0, dv * 1e9 / dt_ns, 0.0)),
      0.0
    ),
    1
  ) AS pgmajfault_s,
  round(
    coalesce(
      sum(iif(name = 'pswpin' AND dt_ns > 0, dv * 1e9 / dt_ns, 0.0)),
      0.0
    ),
    1
  ) AS pswpin_s,
  round(
    coalesce(
      sum(iif(name = 'pswpout' AND dt_ns > 0, dv * 1e9 / dt_ns, 0.0)),
      0.0
    ),
    1
  ) AS pswpout_s,
  round(
    coalesce(
      sum(
        iif(
          name IN ('pgscan_direct', 'pgscan_kswapd')
          AND dt_ns > 0,
          dv * 1e9 / dt_ns,
          0.0
        )
      ),
      0.0
    ),
    1
  ) AS reclaim_scan_s
FROM step
WHERE
  dt_ns IS NOT NULL
  AND dt_ns > 0
  AND rel_sec >= 0
GROUP BY
  rel_sec
ORDER BY
  rel_sec;

-- Reclaim, swap, compaction, PSI, and LMKD summary.
CREATE PERFETTO FUNCTION sysmon_reclaim_summary(start_ts LONG, end_ts LONG)
RETURNS TABLE(
  psi_mem_some_pct DOUBLE,
  psi_mem_full_pct DOUBLE,
  direct_reclaim_events LONG,
  kswapd_wakes LONG,
  allocstall_delta LONG,
  compact_stall_delta LONG,
  pgmajfault_delta LONG,
  pswpin_delta LONG,
  pswpout_delta LONG,
  lmkd_kills LONG,
  active_file_mib DOUBLE,
  inactive_file_mib DOUBLE,
  active_anon_mib DOUBLE,
  inactive_anon_mib DOUBLE
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  unbrushed AS (SELECT iif($start_ts IS NULL AND $end_ts IS NULL, 1, 0) AS flag),
  wt AS MATERIALIZED (SELECT * FROM _sysmon_window_tracks($start_ts, $end_ts)),
  vm AS (
    SELECT
      sum(iif(ct.name GLOB 'allocstall*', max(0.0, wt.delta_v), 0.0)) AS allocstall,
      sum(iif(ct.name = 'compact_stall', max(0.0, wt.delta_v), 0.0)) AS compact_stall,
      sum(iif(ct.name = 'pgmajfault', max(0.0, wt.delta_v), 0.0)) AS pgmajfault,
      sum(iif(ct.name = 'pswpin', max(0.0, wt.delta_v), 0.0)) AS pswpin,
      sum(iif(ct.name = 'pswpout', max(0.0, wt.delta_v), 0.0)) AS pswpout
    FROM counter_track ct
    JOIN wt ON wt.track_id = ct.id
    WHERE
      ct.type = 'vmstat'
  ),
  mi AS (
    SELECT
      max(iif(ct.name = 'Active(file)', wt.last_v, NULL)) / 1048576.0 AS active_file_mib,
      max(iif(ct.name = 'Inactive(file)', wt.last_v, NULL)) / 1048576.0 AS inactive_file_mib,
      max(iif(ct.name = 'Active(anon)', wt.last_v, NULL)) / 1048576.0 AS active_anon_mib,
      max(iif(ct.name = 'Inactive(anon)', wt.last_v, NULL)) / 1048576.0 AS inactive_anon_mib
    FROM counter_track ct
    JOIN wt ON wt.track_id = ct.id
    WHERE
      ct.type = 'meminfo'
      AND ct.name IN (
        'Active(file)',
        'Inactive(file)',
        'Active(anon)',
        'Inactive(anon)'
      )
  ),
  psi AS (
    SELECT
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
      ) AS mem_full
    FROM counter_track ct
    JOIN wt ON wt.track_id = ct.id
    WHERE
      ct.type = 'psi'
  )
SELECT
  psi.mem_some AS psi_mem_some_pct,
  psi.mem_full AS psi_mem_full_pct,
  iif(
    u.flag = 1,
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
      FROM ftrace_event fe
      WHERE
        fe.name = 'mm_vmscan_direct_reclaim_begin'
        AND fe.ts BETWEEN w.start_ts AND w.end_ts
    )
  ) AS direct_reclaim_events,
  iif(
    u.flag = 1,
    coalesce(
      (
        SELECT sum(cnt)
        FROM _sysmon_ftrace_counts
        WHERE
          name = 'mm_vmscan_kswapd_wake'
      ),
      0
    ),
    (
      SELECT count(*)
      FROM ftrace_event fe
      WHERE
        fe.name = 'mm_vmscan_kswapd_wake'
        AND fe.ts BETWEEN w.start_ts AND w.end_ts
    )
  ) AS kswapd_wakes,
  cast_int!(coalesce(vm.allocstall, 0)) AS allocstall_delta,
  cast_int!(coalesce(vm.compact_stall, 0)) AS compact_stall_delta,
  cast_int!(coalesce(vm.pgmajfault, 0)) AS pgmajfault_delta,
  cast_int!(coalesce(vm.pswpin, 0)) AS pswpin_delta,
  cast_int!(coalesce(vm.pswpout, 0)) AS pswpout_delta,
  iif(
    u.flag = 1,
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
      FROM ftrace_event fe
      WHERE
        fe.name IN ('lowmemory_kill', 'mark_victim')
        AND fe.ts BETWEEN w.start_ts AND w.end_ts
    )
  ) AS lmkd_kills,
  round(mi.active_file_mib, 1) AS active_file_mib,
  round(mi.inactive_file_mib, 1) AS inactive_file_mib,
  round(mi.active_anon_mib, 1) AS active_anon_mib,
  round(mi.inactive_anon_mib, 1) AS inactive_anon_mib
FROM w, unbrushed u, vm, mi, psi;

-- Discrete LMKD / OOM kill events.
CREATE PERFETTO FUNCTION sysmon_lmkd_kills(start_ts LONG, end_ts LONG)
RETURNS TABLE(
  ts LONG,
  rel_s DOUBLE,
  pid LONG,
  process_name STRING,
  oom_score_adj LONG,
  rss_mib DOUBLE
)
AS
WITH w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts))
SELECT
  fe.ts,
  round((fe.ts - w.start_ts) / 1e9, 2) AS rel_s,
  coalesce(extract_arg(fe.arg_set_id, 'pid'), p.pid) AS pid,
  coalesce(
    extract_arg(fe.arg_set_id, 'comm'),
    p.name,
    'pid:' || coalesce(extract_arg(fe.arg_set_id, 'pid'), 0)
  ) AS process_name,
  extract_arg(fe.arg_set_id, 'oom_score_adj') AS oom_score_adj,
  round(
    coalesce(extract_arg(fe.arg_set_id, 'rss_in_pages'), 0) * 4096.0 / 1048576.0,
    1
  ) AS rss_mib
FROM ftrace_event fe
JOIN w
LEFT JOIN thread t ON fe.utid = t.utid
LEFT JOIN process p ON t.upid = p.upid
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
ORDER BY
  fe.ts DESC;

-- Top processes holding memory (Anon + Swap + File + DMA-BUF + window growth),
-- powered by _sysmon_window_tracks with zero window-function sorts.
CREATE PERFETTO FUNCTION sysmon_top_memory_processes(
  start_ts LONG,
  end_ts LONG,
  lim LONG
)
RETURNS TABLE(
  upid LONG,
  pid LONG,
  process_name STRING,
  anon_mib DOUBLE,
  swap_mib DOUBLE,
  file_mib DOUBLE,
  rss_mib DOUBLE,
  dmabuf_mib DOUBLE,
  anon_swap_mib DOUBLE,
  delta_mib DOUBLE,
  oom_score_adj LONG
)
AS
WITH
  wt AS MATERIALIZED (SELECT * FROM _sysmon_window_tracks($start_ts, $end_ts)),
  agg AS (
    SELECT
      t.upid,
      coalesce(max(iif(t.name = 'mem.rss.anon', wt.last_v, NULL)), 0)
      / 1048576.0 AS anon_mib,
      coalesce(max(iif(t.name = 'mem.rss.anon', wt.first_v, NULL)), 0)
      / 1048576.0 AS anon_first_mib,
      coalesce(max(iif(t.name = 'mem.swap', wt.last_v, NULL)), 0) / 1048576.0 AS swap_mib,
      coalesce(max(iif(t.name = 'mem.swap', wt.first_v, NULL)), 0) / 1048576.0 AS swap_first_mib,
      coalesce(max(iif(t.name = 'mem.rss.file', wt.last_v, NULL)), 0)
      / 1048576.0 AS file_mib,
      coalesce(max(iif(t.name = 'mem.rss', wt.last_v, NULL)), 0) / 1048576.0 AS rss_mib,
      coalesce(max(iif(t.name = 'mem.dmabuf_rss', wt.last_v, NULL)), 0)
      / 1048576.0 AS dmabuf_mib,
      cast_int!(max(iif(t.name = 'oom_score_adj', wt.last_v, NULL))) AS oom_score_adj
    FROM process_counter_track t
    JOIN wt ON wt.track_id = t.id
    WHERE
      t.name IN (
        'mem.rss.anon',
        'mem.swap',
        'mem.rss.file',
        'mem.rss',
        'mem.dmabuf_rss',
        'oom_score_adj'
      )
    GROUP BY
      t.upid
  )
SELECT
  a.upid,
  p.pid,
  coalesce(p.name, 'pid:' || p.pid) AS process_name,
  round(a.anon_mib, 1) AS anon_mib,
  round(a.swap_mib, 1) AS swap_mib,
  round(a.file_mib, 1) AS file_mib,
  round(max(a.rss_mib, a.anon_mib + a.file_mib), 1) AS rss_mib,
  round(a.dmabuf_mib, 1) AS dmabuf_mib,
  round(a.anon_mib + a.swap_mib, 1) AS anon_swap_mib,
  round((a.anon_mib + a.swap_mib) - (a.anon_first_mib + a.swap_first_mib), 1) AS delta_mib,
  a.oom_score_adj
FROM agg a
JOIN process p USING (upid)
WHERE
  (a.anon_mib + a.swap_mib + a.rss_mib + a.dmabuf_mib) > 0
ORDER BY
  (a.anon_mib + a.swap_mib + a.dmabuf_mib) DESC
LIMIT coalesce($lim, 15);
