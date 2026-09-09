// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// SQL pipeline turning framework `process_state_changed` track events into
// per-process proc-state intervals. Everything is prefixed `_psb_` so this
// plugin never collides with stdlib or other plugins in the global table
// namespace. Only the chain feeding the state timeline is materialized here;
// the wider event families (freezer, broadcast, service/fgs/provider state)
// can be added alongside when tracks exist for them.
export const PROCESS_STATE_SQL = `
-- Raw framework events. proc-state enum args arrive as their string names
-- (e.g. 'PROCESS_STATE_TOP') because trace processor resolves enum-typed
-- track event fields via the descriptor.
CREATE OR REPLACE PERFETTO TABLE _psb_process_state_changed AS
SELECT
  ts,
  extract_arg(arg_set_id, 'process_state_changed_event.uid') AS uid,
  extract_arg(arg_set_id, 'process_state_changed_event.pid') AS pid,
  extract_arg(arg_set_id, 'process_state_changed_event.cur_proc_state')
    AS cur_proc_state
FROM slice
WHERE name = 'process_state_changed';

CREATE OR REPLACE PERFETTO TABLE _psb_process_bound AS
SELECT
  ts,
  extract_arg(arg_set_id, 'process_start_event.uid') AS uid,
  extract_arg(arg_set_id, 'process_start_event.pid') AS pid,
  extract_arg(arg_set_id, 'process_start_event.process_name') AS process_name
FROM slice
WHERE name = 'process_bound';

CREATE OR REPLACE PERFETTO TABLE _psb_process_died AS
SELECT
  ts,
  extract_arg(arg_set_id, 'process_died_event.pid') AS pid,
  extract_arg(arg_set_id, 'process_died_event.reason') AS reason,
  extract_arg(arg_set_id, 'process_died_event.sub_reason') AS sub_reason
FROM slice
WHERE name = 'process_died';

CREATE OR REPLACE PERFETTO TABLE _psb_binder_died AS
SELECT
  ts,
  extract_arg(arg_set_id, 'binder_died_event.pid') AS pid
FROM slice
WHERE name = 'binder_died';

-- Framework-emitted process lifecycle, keyed by upid (one row per process):
-- fw_start_ts from AndroidProcessStartEvent, fw_end_ts from
-- AndroidBinderDiedEvent. Fused below with whatever the process table
-- already has (e.g. from ftrace sched).
CREATE OR REPLACE PERFETTO TABLE _psb_fw_process_lifecycle AS
SELECT upid, fw_start_ts, fw_end_ts
FROM __intrinsic_android_track_event_process;

-- Perceptibility ranking of proc states, mirroring ActivityManager's
-- importance order (NOT the wire enum values: PROCESS_STATE_BOUND_TOP was
-- appended to the proto as 1020 and would sort below the cached states).
-- Lower rank = more perceptible. Unknown/unspecified sort last; states
-- missing from this table (a future enum addition) default to rank 500 at
-- the query sites.
CREATE OR REPLACE PERFETTO TABLE _psb_state_rank AS
WITH r(state, rank) AS (
  VALUES
    ('PROCESS_STATE_PERSISTENT', 0),
    ('PROCESS_STATE_PERSISTENT_UI', 1),
    ('PROCESS_STATE_TOP', 2),
    ('PROCESS_STATE_BOUND_TOP', 3),
    ('PROCESS_STATE_FOREGROUND_SERVICE', 4),
    ('PROCESS_STATE_BOUND_FOREGROUND_SERVICE', 5),
    ('PROCESS_STATE_IMPORTANT_FOREGROUND', 6),
    ('PROCESS_STATE_IMPORTANT_BACKGROUND', 7),
    ('PROCESS_STATE_TRANSIENT_BACKGROUND', 8),
    ('PROCESS_STATE_BACKUP', 9),
    ('PROCESS_STATE_SERVICE', 10),
    ('PROCESS_STATE_RECEIVER', 11),
    ('PROCESS_STATE_TOP_SLEEPING', 12),
    ('PROCESS_STATE_HEAVY_WEIGHT', 13),
    ('PROCESS_STATE_HOME', 14),
    ('PROCESS_STATE_LAST_ACTIVITY', 15),
    ('PROCESS_STATE_CACHED_ACTIVITY', 16),
    ('PROCESS_STATE_CACHED_ACTIVITY_CLIENT', 17),
    ('PROCESS_STATE_CACHED_RECENT', 18),
    ('PROCESS_STATE_CACHED_EMPTY', 19),
    ('PROCESS_STATE_NONEXISTENT', 20),
    ('PROCESS_STATE_UNSPECIFIED', 997),
    ('PROCESS_STATE_UNKNOWN_TO_PROTO', 998),
    ('PROCESS_STATE_UNKNOWN', 999)
)
SELECT
  state,
  rank,
  REPLACE(state, 'PROCESS_STATE_', '') AS display_name
FROM r;

-- Multi-user package mapping: uid % 100000 folds per-user uids (u10 app has
-- uid 1010123) onto the owning package's appid.
CREATE OR REPLACE PERFETTO TABLE _psb_multi_user_mapping AS
WITH all_uids AS (
  SELECT DISTINCT uid FROM process WHERE uid IS NOT NULL
),
collapsed_packages AS (
  SELECT
    uid,
    MIN(package_name) AS package_name,
    MAX(debuggable) AS debuggable,
    MAX(version_code) AS version_code
  FROM package_list
  GROUP BY uid
)
SELECT
  au.uid,
  CASE
    WHEN (au.uid % 100000) = 1000 THEN 'system'
    ELSE pl.package_name
  END AS package_name,
  pl.debuggable,
  pl.version_code
FROM all_uids au
LEFT JOIN collapsed_packages pl ON (au.uid % 100000) = pl.uid;

-- Master process map: one row per process lifecycle (pid can be reused), with
-- start/end fused from the framework lifecycle events, the process table, and
-- binder_died, falling back to the trace bounds.
CREATE OR REPLACE PERFETTO TABLE _psb_process_map AS
WITH _all_starts AS (
  SELECT
    p.pid,
    COALESCE(fw.fw_start_ts, p.start_ts, trace_start()) AS ts,
    p.name AS process_name,
    p.uid,
    p.upid
  FROM process p
  LEFT JOIN _psb_fw_process_lifecycle fw USING (upid)
  WHERE p.pid > 0
  UNION ALL
  SELECT pid, ts, process_name, uid, CAST(NULL AS INT) AS upid
  FROM _psb_process_bound
),
_all_deaths AS (
  SELECT
    p.pid,
    COALESCE(fw.fw_end_ts, p.end_ts) AS ts
  FROM process p
  LEFT JOIN _psb_fw_process_lifecycle fw USING (upid)
  WHERE p.pid > 0 AND COALESCE(fw.fw_end_ts, p.end_ts) IS NOT NULL
  UNION ALL
  SELECT pid, ts FROM _psb_binder_died
),
_starts_with_next_death AS (
  SELECT
    s.pid, s.ts AS start_ts, s.process_name, s.uid, s.upid,
    MIN(d.ts) AS next_death_ts
  FROM _all_starts s
  LEFT JOIN _all_deaths d ON s.pid = d.pid AND d.ts >= s.ts
  GROUP BY s.pid, s.ts, s.process_name, s.uid, s.upid
),
_lifecycles AS (
  SELECT
    pid,
    MIN(start_ts) AS start_ts,
    next_death_ts,
    MAX(upid) AS upid,
    MAX(process_name) AS process_name,
    -- Prefer UID from the process table (has upid) over slice-only entries
    -- to avoid PID-reuse artifacts (e.g. kworker inheriting an app PID).
    COALESCE(MAX(CASE WHEN upid IS NOT NULL THEN uid END), MAX(uid)) AS uid
  FROM _starts_with_next_death
  GROUP BY pid, next_death_ts
)
SELECT
  COALESCE(
    l.upid,
    CAST((ROW_NUMBER() OVER (ORDER BY l.start_ts, l.pid)) + 1000000 AS INT)
  ) AS upid,
  l.pid,
  l.uid,
  mum.package_name,
  mum.debuggable,
  mum.version_code,
  l.process_name,
  l.start_ts,
  COALESCE(l.next_death_ts, trace_end()) AS end_ts,
  (
    SELECT reason FROM _psb_process_died pd
    WHERE pd.pid = l.pid AND pd.ts >= l.start_ts
      AND pd.ts <= COALESCE(l.next_death_ts, trace_end())
    ORDER BY ts ASC LIMIT 1
  ) AS death_reason,
  (
    SELECT sub_reason FROM _psb_process_died pd
    WHERE pd.pid = l.pid AND pd.ts >= l.start_ts
      AND pd.ts <= COALESCE(l.next_death_ts, trace_end())
    ORDER BY ts ASC LIMIT 1
  ) AS death_sub_reason
FROM _lifecycles l
LEFT JOIN _psb_multi_user_mapping mum ON l.uid = mum.uid
WHERE mum.package_name IS NOT NULL;

CREATE OR REPLACE PERFETTO TABLE _psb_process_state_enriched AS
SELECT
  ps.*,
  ap.upid,
  ap.process_name,
  ap.package_name,
  ap.debuggable,
  ap.version_code,
  ap.death_reason,
  ap.death_sub_reason,
  ap.end_ts AS process_end_ts
FROM _psb_process_state_changed ps
JOIN _psb_process_map ap
  ON ps.pid = ap.pid AND ps.ts >= ap.start_ts AND ps.ts < ap.end_ts;

-- The state timeline: one interval per (process lifecycle, run of a state).
-- id is required by BreakdownTracks' sliceIdColumn.
CREATE OR REPLACE PERFETTO TABLE _psb_process_state_intervals AS
WITH state_with_lag AS (
  SELECT
    ts, uid, package_name, debuggable, version_code, pid, process_name,
    death_reason,
    cur_proc_state AS state,
    LAG(cur_proc_state) OVER (PARTITION BY upid ORDER BY ts) AS prev_state,
    process_end_ts, upid
  FROM _psb_process_state_enriched
),
raw_transitions AS (
  SELECT
    ts, uid, package_name, debuggable, version_code, pid, process_name,
    death_reason, state,
    LEAD(ts) OVER (PARTITION BY upid ORDER BY ts) AS next_ts,
    process_end_ts
  FROM state_with_lag
  WHERE prev_state IS NULL OR state != prev_state
),
calculated_durations AS (
  SELECT
    ts,
    COALESCE(next_ts, process_end_ts) - ts AS dur,
    uid, pid, process_name, package_name, debuggable, version_code,
    death_reason, state
  FROM raw_transitions
  WHERE state IS NOT NULL
)
SELECT
  ROW_NUMBER() OVER (ORDER BY ts) AS id,
  *,
  COALESCE(package_name, process_name) || ': ' || uid AS name
FROM calculated_durations
WHERE dur > 0;
`;

// The "most perceptible state" timeline: at every instant, the best (lowest)
// perceptibility rank across ALL tracked processes, as one slice per run.
// _interval_self_intersect_agg computes MIN(rank) over each atomic overlap
// segment in C++ (one pre-aggregated row per segment); the islands pass then
// merges contiguous same-rank segments — the C++ merge alone can't fuse them
// because the process count changing splits segments even when the min
// doesn't. Time with no tracked process alive produces no slice (cnt = 0
// rows are dropped), so gaps stay visibly empty.
export const PERCEPTIBLE_STATE_SQL = `
INCLUDE PERFETTO MODULE intervals.self_intersect;

CREATE OR REPLACE PERFETTO TABLE _psb_perceptible_slices AS
WITH ranked AS (
  SELECT i.ts, i.dur, IFNULL(r.rank, 500) AS v
  FROM _psb_process_state_intervals i
  LEFT JOIN _psb_state_rank r USING (state)
),
covered AS (
  SELECT ts, dur, CAST(min_value AS INT64) AS rank
  FROM _interval_self_intersect_agg!(ranked, v, ())
  WHERE cnt > 0
),
marked AS (
  SELECT
    ts, dur, rank,
    IIF(
      LAG(rank) OVER (ORDER BY ts) = rank
        AND LAG(ts + dur) OVER (ORDER BY ts) = ts,
      0,
      1
    ) AS is_break
  FROM covered
),
runs AS (
  SELECT ts, dur, rank, SUM(is_break) OVER (ORDER BY ts) AS run_id
  FROM marked
)
SELECT
  run_id AS id,
  MIN(ts) AS ts,
  MAX(ts + dur) - MIN(ts) AS dur,
  MIN(rank) AS rank
FROM runs
GROUP BY run_id;
`;
