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

-- Shared OOM bucket index matching Memscope's OOM_SCORE_BUCKETS palette.
CREATE PERFETTO FUNCTION sysmon_oom_bucket_idx(oom LONG)
RETURNS LONG
AS
SELECT
  CASE
    WHEN $oom IS NULL THEN -1
    WHEN $oom < 0 THEN 0
    WHEN $oom = 0 THEN 1
    WHEN $oom <= 99 THEN 2
    WHEN $oom <= 299 THEN 3
    WHEN $oom <= 599 THEN 4
    WHEN $oom <= 899 THEN 5
    ELSE 6
  END;

CREATE PERFETTO FUNCTION sysmon_oom_bucket_name(idx LONG)
RETURNS STRING
AS
SELECT
  CASE $idx
    WHEN 0 THEN 'Native (< 0)'
    WHEN 1 THEN 'Foreground (0)'
    WHEN 2 THEN 'Visible (1-99)'
    WHEN 3 THEN 'Perceptible (100-299)'
    WHEN 4 THEN 'Service (300-599)'
    WHEN 5 THEN 'Cached (600-899)'
    WHEN 6 THEN 'Cached (900+)'
    ELSE 'Unknown'
  END;

-- Extract package name from a wm_set_resumed_activity android_logs message
-- (e.g. `User=0 Component="com.example.app/.MainActivity" Reason=...`).
CREATE PERFETTO FUNCTION sysmon_extract_wm_package(msg STRING)
RETURNS STRING
AS
WITH
  after_comp AS (
    SELECT
      CASE
        WHEN instr($msg, 'Component="') > 0 THEN substr(
          $msg,
          instr($msg, 'Component="') + 11
        )
        WHEN instr($msg, ',') > 0 THEN trim(str_split($msg, ',', 1))
        ELSE $msg
      END AS s
  )
SELECT
  CASE
    WHEN instr(s, '/') > 0 THEN substr(s, 1, instr(s, '/') - 1)
    WHEN instr(s, '"') > 0 THEN substr(s, 1, instr(s, '"') - 1)
    ELSE s
  END
FROM after_comp;

-- Top/foreground app and OOM-state process counts for the Overview scorecard.
CREATE PERFETTO FUNCTION sysmon_top_app(start_ts LONG, end_ts LONG)
RETURNS TABLE(
  top_app STRING,
  top_upid LONG,
  top_dur_s DOUBLE,
  source STRING,
  fg_count LONG,
  vis_count LONG,
  perc_count LONG,
  svc_count LONG,
  cached_count LONG,
  native_count LONG,
  lmkd_kills LONG
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  wt AS MATERIALIZED (SELECT * FROM _sysmon_window_tracks($start_ts, $end_ts)),
  proc_latest AS MATERIALIZED (
    SELECT
      t.upid,
      cast_int!(max(iif(t.name = 'oom_score_adj', wt.last_v, NULL))) AS oom,
      min(iif(t.name = 'oom_score_adj', wt.min_ts, NULL)) AS oom_min_ts,
      coalesce(max(iif(t.name = 'mem.rss.anon', wt.last_v, NULL)), 0.0)
      + coalesce(max(iif(t.name = 'mem.swap', wt.last_v, NULL)), 0.0) AS anon_swap,
      sum(iif(t.type = 'proc_stat_runtime', max(0.0, wt.delta_v), 0.0)) AS cpu_ns
    FROM process_counter_track t
    JOIN wt ON wt.track_id = t.id
    WHERE
      t.name IN ('oom_score_adj', 'mem.rss.anon', 'mem.swap')
      OR t.type = 'proc_stat_runtime'
    GROUP BY
      t.upid
  ),
  wm_latest AS (
    SELECT al.ts, sysmon_extract_wm_package(al.msg) AS pkg
    FROM android_logs al, w
    WHERE
      al.tag = 'wm_set_resumed_activity'
      AND al.ts BETWEEN w.start_ts AND w.end_ts
    ORDER BY
      al.ts DESC
    LIMIT 1
  ),
  oom_fg AS (
    SELECT
      pl.upid,
      coalesce(p.name, 'pid:' || p.pid) AS pkg,
      (w.end_ts - coalesce(pl.oom_min_ts, w.start_ts)) / 1e9 AS dur_s
    FROM proc_latest pl
    JOIN process p USING (upid)
    JOIN w
    WHERE
      pl.oom = 0
    ORDER BY
      iif(p.name GLOB '*.*', 1, 0) DESC,
      iif(
        p.name GLOB '*launcher*'
        OR p.name GLOB '*systemui*'
        OR p.name GLOB '*.gms*'
        OR p.name GLOB '*adservices*'
        OR p.name GLOB '*webview*'
        OR p.name GLOB '*inputmethod*'
        OR p.name GLOB '*providers*',
        0,
        1
      ) DESC,
      (pl.cpu_ns / 1e6 + pl.anon_swap / 1048576.0) DESC
    LIMIT 1
  ),
  counts AS (
    SELECT
      sum(iif(oom = 0, 1, 0)) AS fg_count,
      sum(iif(oom BETWEEN 1 AND 99, 1, 0)) AS vis_count,
      sum(iif(oom BETWEEN 100 AND 299, 1, 0)) AS perc_count,
      sum(iif(oom BETWEEN 300 AND 599, 1, 0)) AS svc_count,
      sum(iif(oom >= 600, 1, 0)) AS cached_count,
      sum(iif(oom < 0, 1, 0)) AS native_count
    FROM proc_latest
    WHERE
      oom IS NOT NULL
  ),
  lmk AS (
    SELECT count(*) AS kills
    FROM ftrace_event fe, w
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
  ),
  wm_upid AS (
    SELECT p.upid
    FROM wm_latest wl
    JOIN process p ON p.name = wl.pkg
    ORDER BY
      p.upid DESC
    LIMIT 1
  )
SELECT
  coalesce((SELECT pkg FROM wm_latest), (SELECT pkg FROM oom_fg)) AS top_app,
  coalesce((SELECT upid FROM wm_upid), (SELECT upid FROM oom_fg)) AS top_upid,
  round(
    coalesce(
      (SELECT max(0.0, (w.end_ts - wl.ts) / 1e9) FROM wm_latest wl, w),
      (SELECT dur_s FROM oom_fg),
      0.0
    ),
    1
  ) AS top_dur_s,
  iif(EXISTS (SELECT 1 FROM wm_latest), 'wm', 'oom') AS source,
  coalesce(c.fg_count, 0) AS fg_count,
  coalesce(c.vis_count, 0) AS vis_count,
  coalesce(c.perc_count, 0) AS perc_count,
  coalesce(c.svc_count, 0) AS svc_count,
  coalesce(c.cached_count, 0) AS cached_count,
  coalesce(c.native_count, 0) AS native_count,
  coalesce(l.kills, 0) AS lmkd_kills
FROM counts c, lmk l;

-- Process count and RAM split by OOM bucket for Overview & Apps ProportionBars.
CREATE PERFETTO FUNCTION sysmon_oom_bucket_summary(start_ts LONG, end_ts LONG)
RETURNS TABLE(
  bucket_idx LONG,
  bucket_name STRING,
  proc_count LONG,
  anon_swap_mib DOUBLE,
  file_mib DOUBLE,
  dmabuf_mib DOUBLE,
  cpu_pct DOUBLE
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  wt AS MATERIALIZED (SELECT * FROM _sysmon_window_tracks($start_ts, $end_ts)),
  proc_agg AS (
    SELECT
      t.upid,
      sysmon_oom_bucket_idx(
        cast_int!(max(iif(t.name = 'oom_score_adj', wt.last_v, NULL)))
      ) AS bidx,
      (coalesce(max(iif(t.name = 'mem.rss.anon', wt.last_v, NULL)), 0.0)
      + coalesce(max(iif(t.name = 'mem.swap', wt.last_v, NULL)), 0.0))
      / 1048576.0 AS anon_swap_mib,
      coalesce(max(iif(t.name = 'mem.rss.file', wt.last_v, NULL)), 0.0)
      / 1048576.0 AS file_mib,
      coalesce(max(iif(t.name = 'mem.dmabuf_rss', wt.last_v, NULL)), 0.0)
      / 1048576.0 AS dmabuf_mib,
      100.0 * sum(iif(t.type = 'proc_stat_runtime', max(0.0, wt.delta_v), 0.0))
      / max(1.0, w.dur_s * 1e9 * w.cpu_count) AS cpu_pct
    FROM process_counter_track t
    JOIN wt ON wt.track_id = t.id
    JOIN w
    WHERE
      t.name IN (
        'oom_score_adj',
        'mem.rss.anon',
        'mem.swap',
        'mem.rss.file',
        'mem.dmabuf_rss'
      )
      OR t.type = 'proc_stat_runtime'
    GROUP BY
      t.upid
  )
SELECT
  bidx AS bucket_idx,
  sysmon_oom_bucket_name(bidx) AS bucket_name,
  count(*) AS proc_count,
  round(sum(anon_swap_mib), 1) AS anon_swap_mib,
  round(sum(file_mib), 1) AS file_mib,
  round(sum(dmabuf_mib), 1) AS dmabuf_mib,
  round(sum(cpu_pct), 1) AS cpu_pct
FROM proc_agg
WHERE
  bidx >= 0
GROUP BY
  bidx
ORDER BY
  bidx;

-- App OOM-state intervals across the window for the Apps swimlane timeline.
-- Prioritizes top app, killed apps, apps that changed state, then privileged
-- and largest-RAM apps up to `lim` lanes.
CREATE PERFETTO FUNCTION sysmon_app_state_intervals(
  start_ts LONG,
  end_ts LONG,
  lim LONG
)
RETURNS TABLE(
  upid LONG,
  pid LONG,
  process_name STRING,
  bucket_idx LONG,
  bucket_name STRING,
  oom_score_adj LONG,
  start_rel_s DOUBLE,
  end_rel_s DOUBLE,
  changed LONG,
  is_top LONG,
  killed LONG,
  frozen LONG
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  s_bounds AS (
    SELECT w.start_ts / 1000000000 AS s0, w.end_ts / 1000000000 AS s1 FROM w
  ),
  top_info AS MATERIALIZED (
    SELECT top_app, top_upid FROM sysmon_top_app($start_ts, $end_ts)
  ),
  wt AS MATERIALIZED (SELECT * FROM _sysmon_window_tracks($start_ts, $end_ts)),
  oom_tracks AS MATERIALIZED (
    SELECT id AS track_id, upid
    FROM process_counter_track
    WHERE
      name = 'oom_score_adj'
  ),
  oom_samples AS MATERIALIZED (
    SELECT
      ot.upid,
      cs.max_ts AS ts,
      cast_int!(c.value) AS oom,
      sysmon_oom_bucket_idx(cast_int!(c.value)) AS bidx
    FROM oom_tracks ot
    JOIN _sysmon_counter_sec cs ON cs.track_id = ot.track_id
    JOIN counter c ON c.id = cs.last_id
    JOIN s_bounds sb
    WHERE
      cs.sec BETWEEN sb.s0 AND sb.s1
  ),
  killed_pids AS MATERIALIZED (
    SELECT DISTINCT cast_int!(extract_arg(fe.arg_set_id, 'pid')) AS pid
    FROM ftrace_event fe, w
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
  mem_rank AS MATERIALIZED (
    SELECT
      t.upid,
      (coalesce(max(iif(t.name = 'mem.rss.anon', wt.last_v, NULL)), 0.0)
      + coalesce(max(iif(t.name = 'mem.swap', wt.last_v, NULL)), 0.0))
      / 1048576.0 AS anon_swap_mib
    FROM process_counter_track t
    JOIN wt ON wt.track_id = t.id
    WHERE
      t.name IN ('mem.rss.anon', 'mem.swap')
    GROUP BY
      t.upid
  ),
  proc_summary AS (
    SELECT
      os.upid,
      p.pid,
      coalesce(p.name, 'pid:' || p.pid) AS process_name,
      min(os.bidx) AS min_bidx,
      max(os.bidx) AS max_bidx,
      iif(min(os.bidx) != max(os.bidx), 1, 0) AS changed,
      iif(
        os.upid = (SELECT top_upid FROM top_info)
        OR p.name = (SELECT top_app FROM top_info),
        1,
        0
      ) AS is_top,
      iif(kp.pid IS NOT NULL, 1, 0) AS killed,
      iif(fu.upid IS NOT NULL, 1, 0) AS frozen,
      coalesce(mr.anon_swap_mib, 0.0) AS anon_swap_mib
    FROM oom_samples os
    JOIN process p ON p.upid = os.upid
    LEFT JOIN killed_pids kp ON kp.pid = p.pid
    LEFT JOIN frozen_upids fu ON fu.upid = os.upid
    LEFT JOIN mem_rank mr ON mr.upid = os.upid
    WHERE
      os.bidx >= 0
    GROUP BY
      os.upid
  ),
  picked AS MATERIALIZED (
    SELECT *
    FROM proc_summary
    WHERE
      min_bidx >= 1
      OR changed = 1
      OR killed = 1
      OR is_top = 1
    ORDER BY
      is_top DESC,
      killed DESC,
      changed DESC,
      min_bidx ASC,
      anon_swap_mib DESC
    LIMIT coalesce($lim, 18)
  ),
  ordered AS (
    SELECT
      os.upid,
      os.ts,
      os.oom,
      os.bidx,
      lag(os.bidx) OVER (PARTITION BY os.upid ORDER BY os.ts) AS prev_bidx,
      lead(os.ts) OVER (PARTITION BY os.upid ORDER BY os.ts) AS next_ts
    FROM oom_samples os
    JOIN picked pk ON pk.upid = os.upid
  ),
  grouped AS (
    SELECT
      upid,
      ts,
      next_ts,
      oom,
      bidx,
      sum(iif(prev_bidx IS NULL OR prev_bidx != bidx, 1, 0)) OVER (
        PARTITION BY
          upid
        ORDER BY ts
      ) AS grp
    FROM ordered
  ),
  spans AS (
    SELECT
      g.upid,
      g.bidx,
      min(g.oom) AS oom,
      min(g.ts) AS seg_start_ts,
      max(coalesce(g.next_ts, w.end_ts)) AS seg_end_ts
    FROM grouped g, w
    GROUP BY
      g.upid,
      g.grp,
      g.bidx
  )
SELECT
  s.upid,
  pk.pid,
  pk.process_name,
  s.bidx AS bucket_idx,
  sysmon_oom_bucket_name(s.bidx) AS bucket_name,
  s.oom AS oom_score_adj,
  round(max(0.0, (s.seg_start_ts - w.start_ts) / 1e9), 2) AS start_rel_s,
  round(
    min(
      w.dur_s,
      max(
        (s.seg_start_ts - w.start_ts) / 1e9 + 0.5,
        (s.seg_end_ts - w.start_ts) / 1e9
      )
    ),
    2
  ) AS end_rel_s,
  pk.changed,
  pk.is_top,
  pk.killed,
  pk.frozen
FROM spans s
JOIN picked pk ON pk.upid = s.upid
JOIN w
ORDER BY
  pk.is_top DESC,
  pk.killed DESC,
  pk.changed DESC,
  pk.min_bidx ASC,
  pk.anon_swap_mib DESC,
  s.seg_start_ts ASC;

-- Stacked RAM by OOM bucket over time (aligned time bins) for Apps tab.
CREATE PERFETTO FUNCTION sysmon_oom_memory_series(start_ts LONG, end_ts LONG)
RETURNS TABLE(
  rel_s DOUBLE,
  bucket_idx LONG,
  bucket_name STRING,
  anon_swap_mib DOUBLE,
  file_mib DOUBLE,
  dmabuf_mib DOUBLE
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  s_bounds AS (
    SELECT
      w.start_ts / 1000000000 AS s0,
      w.end_ts / 1000000000 AS s1,
      max(2, cast_int!(ceil(w.dur_s / 30.0))) AS step_s
    FROM w
  ),
  wt AS MATERIALIZED (SELECT * FROM _sysmon_window_tracks($start_ts, $end_ts)),
  proc_oom AS MATERIALIZED (
    SELECT t.upid, sysmon_oom_bucket_idx(cast_int!(wt.last_v)) AS bidx
    FROM process_counter_track t
    JOIN wt ON wt.track_id = t.id
    WHERE
      t.name = 'oom_score_adj'
  ),
  mem_tracks AS MATERIALIZED (
    SELECT t.id, t.upid, t.name, po.bidx
    FROM process_counter_track t
    JOIN proc_oom po ON po.upid = t.upid
    WHERE
      po.bidx >= 0
      AND t.name IN (
        'mem.rss.anon',
        'mem.swap',
        'mem.rss.file',
        'mem.dmabuf_rss'
      )
  ),
  per_proc_bin AS (
    SELECT
      ((cs.sec - sb.s0) / sb.step_s) * sb.step_s AS rel_sec,
      mt.upid,
      mt.bidx,
      max(iif(mt.name = 'mem.rss.anon', c.value, 0.0)) AS anon_b,
      max(iif(mt.name = 'mem.swap', c.value, 0.0)) AS swap_b,
      max(iif(mt.name = 'mem.rss.file', c.value, 0.0)) AS file_b,
      max(iif(mt.name = 'mem.dmabuf_rss', c.value, 0.0)) AS dma_b
    FROM mem_tracks mt
    JOIN _sysmon_counter_sec cs ON cs.track_id = mt.id
    JOIN counter c ON c.id = cs.last_id
    JOIN s_bounds sb
    WHERE
      cs.sec BETWEEN sb.s0 AND sb.s1
      AND cs.sec >= sb.s0
    GROUP BY
      1,
      mt.upid,
      mt.bidx
  )
SELECT
  cast_double!(rel_sec) AS rel_s,
  bidx AS bucket_idx,
  sysmon_oom_bucket_name(bidx) AS bucket_name,
  round(sum(anon_b + swap_b) / 1048576.0, 1) AS anon_swap_mib,
  round(sum(file_b) / 1048576.0, 1) AS file_mib,
  round(sum(dma_b) / 1048576.0, 1) AS dmabuf_mib
FROM per_proc_bin
GROUP BY
  rel_sec,
  bidx
ORDER BY
  rel_sec,
  bidx;

-- Drill-down series for one OOM bucket: top N processes (+ 'Other').
CREATE PERFETTO FUNCTION sysmon_oom_bucket_proc_series(
  start_ts LONG,
  end_ts LONG,
  bucket_idx LONG,
  top_n LONG
)
RETURNS TABLE(
  rel_s DOUBLE,
  process_name STRING,
  anon_swap_mib DOUBLE,
  file_mib DOUBLE,
  dmabuf_mib DOUBLE
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  s_bounds AS (
    SELECT
      w.start_ts / 1000000000 AS s0,
      w.end_ts / 1000000000 AS s1,
      max(2, cast_int!(ceil(w.dur_s / 30.0))) AS step_s
    FROM w
  ),
  wt AS MATERIALIZED (SELECT * FROM _sysmon_window_tracks($start_ts, $end_ts)),
  bucket_upids AS MATERIALIZED (
    SELECT t.upid
    FROM process_counter_track t
    JOIN wt ON wt.track_id = t.id
    WHERE
      t.name = 'oom_score_adj'
      AND sysmon_oom_bucket_idx(cast_int!(wt.last_v)) = $bucket_idx
  ),
  ranked AS MATERIALIZED (
    SELECT
      t.upid,
      coalesce(p.name, 'pid:' || p.pid) || ' (' || p.pid || ')' AS label,
      sum(wt.last_v) AS score
    FROM process_counter_track t
    JOIN bucket_upids bu ON bu.upid = t.upid
    JOIN wt ON wt.track_id = t.id
    JOIN process p ON p.upid = t.upid
    WHERE
      t.name IN ('mem.rss.anon', 'mem.swap', 'mem.rss.file', 'mem.dmabuf_rss')
    GROUP BY
      t.upid
    ORDER BY
      score DESC
    LIMIT coalesce($top_n, 8)
  ),
  mem_tracks AS MATERIALIZED (
    SELECT t.id, t.upid, t.name, coalesce(r.label, 'Other') AS label
    FROM process_counter_track t
    JOIN bucket_upids bu ON bu.upid = t.upid
    LEFT JOIN ranked r ON r.upid = t.upid
    WHERE
      t.name IN ('mem.rss.anon', 'mem.swap', 'mem.rss.file', 'mem.dmabuf_rss')
  ),
  per_proc_bin AS (
    SELECT
      ((cs.sec - sb.s0) / sb.step_s) * sb.step_s AS rel_sec,
      mt.upid,
      mt.label,
      max(iif(mt.name = 'mem.rss.anon', c.value, 0.0)) AS anon_b,
      max(iif(mt.name = 'mem.swap', c.value, 0.0)) AS swap_b,
      max(iif(mt.name = 'mem.rss.file', c.value, 0.0)) AS file_b,
      max(iif(mt.name = 'mem.dmabuf_rss', c.value, 0.0)) AS dma_b
    FROM mem_tracks mt
    JOIN _sysmon_counter_sec cs ON cs.track_id = mt.id
    JOIN counter c ON c.id = cs.last_id
    JOIN s_bounds sb
    WHERE
      cs.sec BETWEEN sb.s0 AND sb.s1
      AND cs.sec >= sb.s0
    GROUP BY
      1,
      mt.upid,
      mt.label
  )
SELECT
  cast_double!(rel_sec) AS rel_s,
  label AS process_name,
  round(sum(anon_b + swap_b) / 1048576.0, 1) AS anon_swap_mib,
  round(sum(file_b) / 1048576.0, 1) AS file_mib,
  round(sum(dma_b) / 1048576.0, 1) AS dmabuf_mib
FROM per_proc_bin
GROUP BY
  rel_sec,
  label
ORDER BY
  rel_sec,
  label;

-- Chronological notable events feed: LMK kills, top-app switches, OOM state
-- transitions, process starts/deaths, and freezer changes.
CREATE PERFETTO FUNCTION sysmon_app_events(start_ts LONG, end_ts LONG, lim LONG)
RETURNS TABLE(
  rel_s DOUBLE,
  kind STRING,
  severity STRING,
  process_name STRING,
  title STRING,
  detail STRING,
  target_tab STRING
)
AS
WITH
  w AS MATERIALIZED (SELECT * FROM sysmon_window_bounds($start_ts, $end_ts)),
  s_bounds AS (
    SELECT w.start_ts / 1000000000 AS s0, w.end_ts / 1000000000 AS s1 FROM w
  ),
  lmk_ev AS (
    SELECT
      round((fe.ts - w.start_ts) / 1e9, 1) AS rel_s,
      'lmk' AS kind,
      'danger' AS severity,
      coalesce(
        extract_arg(fe.arg_set_id, 'comm'),
        'pid:' || coalesce(extract_arg(fe.arg_set_id, 'pid'), '?')
      ) AS process_name,
      'Killed by LMKD' AS title,
      'OOM adj '
      || coalesce(
        extract_arg(fe.arg_set_id, 'oom_score_adj'),
        extract_arg(fe.arg_set_id, 'adj'),
        '?'
      )
      || ' · '
      || round(
        coalesce(extract_arg(fe.arg_set_id, 'rss_in_pages'), 0) * 4.0 / 1024.0,
        1
      )
      || ' MiB RSS' AS detail,
      'memory' AS target_tab
    FROM ftrace_event fe, w
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
  ),
  log_ev AS (
    SELECT
      round((al.ts - w.start_ts) / 1e9, 1) AS rel_s,
      CASE al.tag
        WHEN 'wm_set_resumed_activity' THEN 'top'
        WHEN 'am_kill' THEN 'lmk'
        WHEN 'lowmemorykiller' THEN 'lmk'
        ELSE 'proc'
      END AS kind,
      CASE al.tag
        WHEN 'am_kill' THEN 'danger'
        WHEN 'lowmemorykiller' THEN 'danger'
        ELSE 'info'
      END AS severity,
      CASE al.tag
        WHEN 'wm_set_resumed_activity' THEN sysmon_extract_wm_package(al.msg)
        ELSE coalesce(p.name, al.tag)
      END AS process_name,
      CASE al.tag
        WHEN 'wm_set_resumed_activity' THEN 'Came to foreground'
        WHEN 'am_kill' THEN 'Killed by ActivityManager'
        WHEN 'lowmemorykiller' THEN 'LMKD kill'
        WHEN 'am_proc_start' THEN 'Process started'
        WHEN 'am_proc_died' THEN 'Process exited'
        ELSE al.tag
      END AS title,
      substr(coalesce(al.msg, ''), 1, 140) AS detail,
      iif(al.tag IN ('am_kill', 'lowmemorykiller'), 'memory', 'apps') AS target_tab
    FROM android_logs al
    JOIN w
    LEFT JOIN thread t ON t.utid = al.utid
    LEFT JOIN process p ON p.upid = t.upid
    WHERE
      al.tag IN (
        'wm_set_resumed_activity',
        'am_proc_start',
        'am_proc_died',
        'am_kill',
        'lowmemorykiller'
      )
      AND al.ts BETWEEN w.start_ts AND w.end_ts
  ),
  oom_tracks AS MATERIALIZED (
    SELECT id AS track_id, upid
    FROM process_counter_track
    WHERE
      name = 'oom_score_adj'
  ),
  oom_steps AS (
    SELECT
      ot.upid,
      cs.max_ts AS ts,
      cast_int!(c.value) AS oom,
      sysmon_oom_bucket_idx(cast_int!(c.value)) AS bidx,
      lag(sysmon_oom_bucket_idx(cast_int!(c.value))) OVER (
        PARTITION BY
          ot.upid
        ORDER BY cs.sec
      ) AS prev_bidx
    FROM oom_tracks ot
    JOIN _sysmon_counter_sec cs ON cs.track_id = ot.track_id
    JOIN counter c ON c.id = cs.last_id
    JOIN s_bounds sb
    WHERE
      cs.sec BETWEEN sb.s0 AND sb.s1
  ),
  oom_ev AS (
    SELECT
      round((os.ts - w.start_ts) / 1e9, 1) AS rel_s,
      'state' AS kind,
      iif(os.bidx = 1, 'info', 'warn') AS severity,
      coalesce(p.name, 'pid:' || p.pid) AS process_name,
      sysmon_oom_bucket_name(os.prev_bidx) || ' → '
      || sysmon_oom_bucket_name(os.bidx) AS title,
      'oom_score_adj → ' || os.oom AS detail,
      'apps' AS target_tab
    FROM oom_steps os
    JOIN process p ON p.upid = os.upid
    JOIN w
    WHERE
      os.prev_bidx IS NOT NULL
      AND os.prev_bidx != os.bidx
      AND os.bidx >= 0
      AND os.prev_bidx >= 0
  ),
  all_ev AS (
    SELECT * FROM lmk_ev
    UNION ALL
    SELECT * FROM log_ev
    UNION ALL
    SELECT * FROM oom_ev
  )
SELECT * FROM all_ev ORDER BY rel_s DESC LIMIT coalesce($lim, 30);
