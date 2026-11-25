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

-- Whole-device storage summary (works from diskstats + vmstat + PSI in Live,
-- and enriches with block_rq_issue + D-state io_wait in Snapshot).
CREATE PERFETTO FUNCTION sysmon_storage_summary(start_ts LONG, end_ts LONG)
RETURNS TABLE(
  dur_s DOUBLE,
  read_mb DOUBLE,
  write_mb DOUBLE,
  read_mb_s DOUBLE,
  write_mb_s DOUBLE,
  block_ops LONG,
  io_psi_some_pct DOUBLE,
  io_psi_full_pct DOUBLE,
  io_dstate_ms DOUBLE,
  dirty_mib DOUBLE,
  writeback_mib DOUBLE,
  pgpgin_mb DOUBLE,
  pgpgout_mb DOUBLE,
  swap_io_mb DOUBLE
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  unbrushed AS (SELECT iif($start_ts IS NULL AND $end_ts IS NULL, 1, 0) AS flag),
  wt AS MATERIALIZED (SELECT * FROM _sysmon_window_tracks($start_ts, $end_ts)),
  disk_tracks AS MATERIALIZED (
    SELECT id, name
    FROM counter_track
    WHERE
      type = 'diskstat'
      AND (name GLOB 'diskstat.*.read_amount'
      OR name GLOB 'diskstat.*.write_amount')
  ),
  ds AS (
    SELECT
      sum(iif(dt.name GLOB 'diskstat.*.read_amount', c.value, 0.0)) AS read_mb,
      sum(iif(dt.name GLOB 'diskstat.*.write_amount', c.value, 0.0)) AS write_mb
    FROM disk_tracks dt
    JOIN counter c ON c.track_id = dt.id
    JOIN w
    WHERE
      c.ts BETWEEN w.start_ts AND w.end_ts
  ),
  blk AS (
    SELECT
      iif(
        w.has_block = 0,
        0,
        iif(
          u.flag = 1,
          coalesce(
            (
              SELECT sum(cnt)
              FROM _sysmon_ftrace_counts
              WHERE
                name = 'block_rq_issue'
            ),
            0
          ),
          (
            SELECT count(*)
            FROM ftrace_event fe
            WHERE
              fe.name = 'block_rq_issue'
              AND fe.ts BETWEEN w.start_ts AND w.end_ts
          )
        )
      ) AS ops,
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
    FROM w, unbrushed u
  ),
  mi AS (
    SELECT
      coalesce(max(iif(ct.name = 'Dirty', wt.last_v, NULL)), 0.0) / 1048576.0 AS dirty_mib,
      coalesce(max(iif(ct.name = 'Writeback', wt.last_v, NULL)), 0.0)
      / 1048576.0 AS writeback_mib
    FROM counter_track ct
    JOIN wt ON wt.track_id = ct.id
    WHERE
      ct.type = 'meminfo'
      AND ct.name IN ('Dirty', 'Writeback')
  ),
  vm AS (
    SELECT
      coalesce(max(iif(ct.name = 'pgpgin', max(0.0, wt.delta_v), NULL)), 0.0) AS pgpgin,
      coalesce(max(iif(ct.name = 'pgpgout', max(0.0, wt.delta_v), NULL)), 0.0) AS pgpgout,
      coalesce(max(iif(ct.name = 'pswpin', max(0.0, wt.delta_v), NULL)), 0.0) AS pswpin,
      coalesce(max(iif(ct.name = 'pswpout', max(0.0, wt.delta_v), NULL)), 0.0) AS pswpout
    FROM counter_track ct
    JOIN wt ON wt.track_id = ct.id
    WHERE
      ct.type = 'vmstat'
      AND ct.name IN ('pgpgin', 'pgpgout', 'pswpin', 'pswpout')
  ),
  psi AS (
    SELECT
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
      ) AS ms
  ),
  chosen AS (
    SELECT
      coalesce(iif(w.has_diskstat = 1, ds.read_mb, NULL), blk.read_mb, 0.0) AS r_mb,
      coalesce(iif(w.has_diskstat = 1, ds.write_mb, NULL), blk.write_mb, 0.0) AS w_mb
    FROM w, ds, blk
  )
SELECT
  round(w.dur_s, 2) AS dur_s,
  round(c.r_mb, 2) AS read_mb,
  round(c.w_mb, 2) AS write_mb,
  round(c.r_mb / w.dur_s, 2) AS read_mb_s,
  round(c.w_mb / w.dur_s, 2) AS write_mb_s,
  blk.ops AS block_ops,
  psi.io_some AS io_psi_some_pct,
  psi.io_full AS io_psi_full_pct,
  round(dstate.ms, 1) AS io_dstate_ms,
  round(mi.dirty_mib, 2) AS dirty_mib,
  round(mi.writeback_mib, 2) AS writeback_mib,
  round(vm.pgpgin / 1024.0, 2) AS pgpgin_mb,
  round(vm.pgpgout / 1024.0, 2) AS pgpgout_mb,
  round((vm.pswpin + vm.pswpout) * 4096.0 / 1048576.0, 2) AS swap_io_mb
FROM w, chosen c, blk, dstate, mi, vm, psi;

-- Storage throughput, IO PSI (some/full), and Dirty/Writeback MiB time series
-- at 1s resolution.
CREATE PERFETTO FUNCTION sysmon_storage_series(start_ts LONG, end_ts LONG)
RETURNS TABLE(
  rel_s DOUBLE,
  read_mb_s DOUBLE,
  write_mb_s DOUBLE,
  io_psi_pct DOUBLE,
  io_psi_full_pct DOUBLE,
  dirty_mib DOUBLE,
  writeback_mib DOUBLE
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  start_sec AS (
    SELECT w.start_ts / 1000000000 AS s0, w.end_ts / 1000000000 AS s1 FROM w
  ),
  disk_tracks AS MATERIALIZED (
    SELECT id, name
    FROM counter_track
    WHERE
      type = 'diskstat'
      AND (name GLOB 'diskstat.*.read_amount'
      OR name GLOB 'diskstat.*.write_amount')
  ),
  ds_sec AS (
    SELECT
      cast_int!((c.ts - w.start_ts) / 1e9) AS rel_sec,
      sum(iif(dt.name GLOB 'diskstat.*.read_amount', c.value, 0.0)) AS r_mb,
      sum(iif(dt.name GLOB 'diskstat.*.write_amount', c.value, 0.0)) AS w_mb
    FROM disk_tracks dt
    JOIN counter c ON c.track_id = dt.id
    JOIN w
    WHERE
      c.ts BETWEEN w.start_ts AND w.end_ts
    GROUP BY
      1
  ),
  blk_sec AS (
    SELECT
      cast_int!((fe.ts - w.start_ts) / 1e9) AS rel_sec,
      sum(
        iif(
          coalesce(extract_arg(fe.arg_set_id, 'rwbs'), '') GLOB '*W*',
          0.0,
          coalesce(extract_arg(fe.arg_set_id, 'nr_sector'), 0) * 512.0
          / 1048576.0
        )
      ) AS r_mb,
      sum(
        iif(
          coalesce(extract_arg(fe.arg_set_id, 'rwbs'), '') GLOB '*W*',
          coalesce(extract_arg(fe.arg_set_id, 'nr_sector'), 0) * 512.0
          / 1048576.0,
          0.0
        )
      ) AS w_mb
    FROM ftrace_event fe
    JOIN w
    WHERE
      w.has_diskstat = 0
      AND w.has_block = 1
      AND fe.name = 'block_rq_issue'
      AND fe.ts BETWEEN w.start_ts AND w.end_ts
    GROUP BY
      1
  ),
  psi_tracks AS MATERIALIZED (
    SELECT id, name
    FROM counter_track
    WHERE
      type = 'psi'
      AND name IN ('psi.io.some', 'psi.io.full')
  ),
  psi_step AS (
    SELECT
      cs.sec - ss.s0 AS rel_sec,
      pt.name,
      100.0
      * max(
        0.0,
        c.value - lag(c.value) OVER (PARTITION BY cs.track_id ORDER BY cs.sec)
      )
      / max(
        1.0,
        cs.max_ts
        - lag(cs.max_ts) OVER (PARTITION BY cs.track_id ORDER BY cs.sec)
      ) AS pct
    FROM psi_tracks pt
    JOIN _sysmon_counter_sec cs ON cs.track_id = pt.id
    JOIN counter c ON c.id = cs.last_id
    JOIN start_sec ss
    WHERE
      cs.sec BETWEEN ss.s0 AND ss.s1
  ),
  psi_sec AS (
    SELECT
      rel_sec,
      coalesce(max(iif(name = 'psi.io.some', pct, NULL)), 0.0) AS psi_some,
      coalesce(max(iif(name = 'psi.io.full', pct, NULL)), 0.0) AS psi_full
    FROM psi_step
    WHERE
      pct IS NOT NULL
    GROUP BY
      rel_sec
  ),
  dw_tracks AS MATERIALIZED (
    SELECT id, name
    FROM counter_track
    WHERE
      type = 'meminfo'
      AND name IN ('Dirty', 'Writeback')
  ),
  dw_sec AS (
    SELECT
      cs.sec - ss.s0 AS rel_sec,
      coalesce(max(iif(dt.name = 'Dirty', c.value, NULL)), 0.0) / 1048576.0 AS dirty_mib,
      coalesce(max(iif(dt.name = 'Writeback', c.value, NULL)), 0.0) / 1048576.0 AS writeback_mib
    FROM dw_tracks dt
    JOIN _sysmon_counter_sec cs ON cs.track_id = dt.id
    JOIN counter c ON c.id = cs.last_id
    JOIN start_sec ss
    WHERE
      cs.sec BETWEEN ss.s0 AND ss.s1
    GROUP BY
      cs.sec
  ),
  all_secs AS (
    SELECT rel_sec FROM ds_sec
    UNION
    SELECT rel_sec FROM blk_sec
    UNION
    SELECT rel_sec FROM psi_sec
    UNION
    SELECT rel_sec FROM dw_sec
  )
SELECT
  cast_double!(a.rel_sec) AS rel_s,
  round(coalesce(ds.r_mb, blk.r_mb, 0.0), 3) AS read_mb_s,
  round(coalesce(ds.w_mb, blk.w_mb, 0.0), 3) AS write_mb_s,
  round(coalesce(ps.psi_some, 0.0), 2) AS io_psi_pct,
  round(coalesce(ps.psi_full, 0.0), 2) AS io_psi_full_pct,
  round(coalesce(dw.dirty_mib, 0.0), 2) AS dirty_mib,
  round(coalesce(dw.writeback_mib, 0.0), 2) AS writeback_mib
FROM all_secs a
LEFT JOIN ds_sec ds USING (rel_sec)
LEFT JOIN blk_sec blk USING (rel_sec)
LEFT JOIN psi_sec ps USING (rel_sec)
LEFT JOIN dw_sec dw USING (rel_sec)
WHERE
  a.rel_sec >= 0
ORDER BY
  a.rel_sec;

-- Per-process storage I/O and D-state I/O wait (populated from block_rq_issue
-- and thread_state in Snapshot).
CREATE PERFETTO FUNCTION sysmon_storage_processes(
  start_ts LONG,
  end_ts LONG,
  lim LONG
)
RETURNS TABLE(
  upid LONG,
  process_name STRING,
  read_mb DOUBLE,
  write_mb DOUBLE,
  total_io_mb DOUBLE,
  block_ops LONG,
  io_wait_ms DOUBLE
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  unbrushed AS (SELECT iif($start_ts IS NULL AND $end_ts IS NULL, 1, 0) AS flag),
  blk AS (
    SELECT
      coalesce(t.upid, -1) AS upid,
      coalesce(p.name, t.name, 'kernel') AS process_name,
      count(*) AS ops,
      sum(
        iif(
          coalesce(extract_arg(fe.arg_set_id, 'rwbs'), '') GLOB '*W*',
          0.0,
          coalesce(extract_arg(fe.arg_set_id, 'nr_sector'), 0) * 512.0
          / 1048576.0
        )
      ) AS read_mb,
      sum(
        iif(
          coalesce(extract_arg(fe.arg_set_id, 'rwbs'), '') GLOB '*W*',
          coalesce(extract_arg(fe.arg_set_id, 'nr_sector'), 0) * 512.0
          / 1048576.0,
          0.0
        )
      ) AS write_mb
    FROM ftrace_event fe
    JOIN w
    LEFT JOIN thread t ON fe.utid = t.utid
    LEFT JOIN process p ON t.upid = p.upid
    WHERE
      w.has_block = 1
      AND fe.name = 'block_rq_issue'
      AND fe.ts BETWEEN w.start_ts AND w.end_ts
    GROUP BY
      1,
      2
  ),
  dwait AS (
    SELECT
      coalesce(t.upid, -1) AS upid,
      coalesce(p.name, t.name, 'kernel') AS process_name,
      sum(a.total_ns) / 1e6 AS io_wait_ms
    FROM _sysmon_thread_state_agg a
    JOIN thread t ON a.utid = t.utid
    LEFT JOIN process p ON t.upid = p.upid
    WHERE
      $start_ts IS NULL
      AND $end_ts IS NULL
      AND a.state = 'D'
      AND a.io_wait = 1
    GROUP BY
      1,
      2
    UNION ALL
    SELECT
      coalesce(t.upid, -1) AS upid,
      coalesce(p.name, t.name, 'kernel') AS process_name,
      sum(max(0, min(ts.ts + ts.dur, w.end_ts) - max(ts.ts, w.start_ts))) / 1e6 AS io_wait_ms
    FROM thread_state ts
    JOIN w
    JOIN thread t ON ts.utid = t.utid
    LEFT JOIN process p ON t.upid = p.upid
    WHERE
      ($start_ts IS NOT NULL OR $end_ts IS NOT NULL)
      AND ts.state = 'D'
      AND ts.io_wait = 1
      AND ts.dur > 0
      AND ts.ts < w.end_ts
      AND ts.ts + ts.dur > w.start_ts
    GROUP BY
      1,
      2
  ),
  keys AS (
    SELECT upid, process_name FROM blk
    UNION
    SELECT upid, process_name FROM dwait
  )
SELECT
  k.upid,
  k.process_name,
  round(coalesce(b.read_mb, 0.0), 2) AS read_mb,
  round(coalesce(b.write_mb, 0.0), 2) AS write_mb,
  round(coalesce(b.read_mb, 0.0) + coalesce(b.write_mb, 0.0), 2) AS total_io_mb,
  coalesce(b.ops, 0) AS block_ops,
  round(coalesce(d.io_wait_ms, 0.0), 1) AS io_wait_ms
FROM keys k
LEFT JOIN blk b ON k.upid = b.upid AND k.process_name = b.process_name
LEFT JOIN dwait d ON k.upid = d.upid AND k.process_name = d.process_name
ORDER BY
  (coalesce(b.read_mb, 0.0) + coalesce(b.write_mb, 0.0)
  + coalesce(d.io_wait_ms, 0.0) / 10.0) DESC
LIMIT coalesce($lim, 15);
