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
--

INCLUDE PERFETTO MODULE android.process_metadata;

INCLUDE PERFETTO MODULE intervals.overlap;

-- Ordering follows the declaration order of ProcessStateEnum in
-- protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_track_event.proto
-- (lines 317-342).
-- Numeric values do not match declaration order because
-- PROCESS_STATE_BOUND_TOP was appended as 1020 while declared between
-- TOP (1002) and FOREGROUND_SERVICE (1003).
-- Explicit unknown states (UNSPECIFIED, UNKNOWN_TO_PROTO, UNKNOWN) sort last.
CREATE PERFETTO TABLE _android_process_state_rank AS
WITH
  r(state, rank) AS (
    VALUES
      ('PROCESS_STATE_PERSISTENT', 0), ('PROCESS_STATE_PERSISTENT_UI', 1),
      (
        'PROCESS_STATE_TOP',
        2
      ), ('PROCESS_STATE_BOUND_TOP', 3), ('PROCESS_STATE_FOREGROUND_SERVICE', 4),
      (
        'PROCESS_STATE_BOUND_FOREGROUND_SERVICE',
        5
      ), ('PROCESS_STATE_IMPORTANT_FOREGROUND', 6),
      (
        'PROCESS_STATE_IMPORTANT_BACKGROUND',
        7
      ), ('PROCESS_STATE_TRANSIENT_BACKGROUND', 8), ('PROCESS_STATE_BACKUP', 9),
      (
        'PROCESS_STATE_SERVICE',
        10
      ), ('PROCESS_STATE_RECEIVER', 11), ('PROCESS_STATE_TOP_SLEEPING', 12),
      (
        'PROCESS_STATE_HEAVY_WEIGHT',
        13
      ), ('PROCESS_STATE_HOME', 14), ('PROCESS_STATE_LAST_ACTIVITY', 15),
      (
        'PROCESS_STATE_CACHED_ACTIVITY',
        16
      ), ('PROCESS_STATE_CACHED_ACTIVITY_CLIENT', 17),
      (
        'PROCESS_STATE_CACHED_RECENT',
        18
      ), ('PROCESS_STATE_CACHED_EMPTY', 19), ('PROCESS_STATE_NONEXISTENT', 20),
      (
        'EXITED',
        21
      ), ('PROCESS_STATE_UNSPECIFIED', 997),
      (
        'PROCESS_STATE_UNKNOWN_TO_PROTO',
        998
      ), ('PROCESS_STATE_UNKNOWN', 999)
  )
SELECT replace(state, 'PROCESS_STATE_', '') AS state, rank FROM r;

-- Intervals of Android framework process states.
-- One interval per state a process was in.
CREATE PERFETTO TABLE _android_process_state_intervals AS
WITH
  process_lifetimes AS MATERIALIZED (
    SELECT
      p.upid,
      -- AndroidProcessStartEvent only sets fw_start_ts; process.start_ts comes
      -- from ftrace forks.
      coalesce(fw.fw_start_ts, p.start_ts, trace_start()) AS start_ts,
      p.end_ts AS death_ts,
      s.first_ts,
      s.last_ts
    FROM (
      SELECT upid, min(ts) AS first_ts, max(ts) AS last_ts
      FROM __intrinsic_android_process_state
      GROUP BY
        upid
    ) AS s
    JOIN process AS p USING (upid)
    LEFT JOIN __intrinsic_android_track_event_process AS fw USING (upid)
  ),
  -- Most rows only change the OOM score: drop repeated states before the
  -- join and sort below.
  deduped AS MATERIALIZED (
    SELECT ts, is_initial, upid, proc_state, reason
    FROM (
      SELECT
        ts,
        is_initial,
        upid,
        proc_state,
        reason,
        lag(proc_state) OVER (PARTITION BY upid ORDER BY ts, is_initial DESC) AS prev_proc_state
      FROM __intrinsic_android_process_state
    )
    WHERE
      prev_proc_state IS NULL
      OR proc_state IS NULL
      OR prev_proc_state != proc_state
  ),
  -- Rows outside the lifetime interleave with the synthesized ones below, so
  -- these processes keep all of their rows.
  out_of_lifetime AS (
    SELECT upid
    FROM process_lifetimes
    WHERE
      first_ts < start_ts
      OR last_ts >= death_ts
  ),
  state_events AS (
    SELECT * FROM deduped WHERE upid NOT IN (SELECT upid FROM out_of_lifetime)
    UNION ALL
    SELECT s.ts, s.is_initial, s.upid, s.proc_state, s.reason
    FROM out_of_lifetime AS o
    CROSS JOIN __intrinsic_android_process_state AS s
    WHERE
      s.upid = o.upid
  ),
  all_events AS (
    SELECT
      coalesce(s.ts, p.start_ts) AS ts,
      s.is_initial,
      s.upid,
      replace(s.proc_state, 'PROCESS_STATE_', '') AS cur_state,
      s.reason AS cur_reason
    FROM state_events AS s
    JOIN process_lifetimes AS p USING (upid)
    UNION ALL
    -- Death is never reported by the state stream, so synthesize it. Left open
    -- ended (dur = -1), as the process never comes back.
    SELECT
      p.death_ts AS ts,
      0 AS is_initial,
      p.upid,
      'EXITED' AS cur_state,
      NULL AS cur_reason
    FROM process_lifetimes AS p
    WHERE
      p.death_ts IS NOT NULL
  ),
  raw_changes AS (
    SELECT
      e.ts,
      e.is_initial,
      e.upid,
      e.cur_state,
      lag(e.cur_state) OVER (
        PARTITION BY
          e.upid
        ORDER BY e.ts, e.is_initial DESC
      ) AS prev_state,
      e.cur_reason
    FROM all_events AS e
  ),
  state_changes AS (
    SELECT
      c.ts,
      coalesce(
        max(
          lead(c.ts) OVER (PARTITION BY c.upid ORDER BY c.ts, c.is_initial DESC)
          - c.ts,
          0
        ),
        -1
      ) AS dur,
      c.upid,
      c.cur_state AS state,
      c.prev_state,
      c.ts
      - lag(c.ts) OVER (PARTITION BY c.upid ORDER BY c.ts, c.is_initial DESC) AS prev_state_duration,
      c.cur_reason AS state_change_reason
    FROM raw_changes AS c
    WHERE
      c.prev_state IS NULL
      OR c.prev_state != c.cur_state
  )
SELECT
  row_number() OVER (ORDER BY c.ts, c.upid) AS id,
  c.ts,
  c.dur,
  c.upid,
  p.pid,
  m.uid,
  m.user_id,
  m.process_name,
  m.package_name,
  m.version_code,
  m.debuggable,
  c.state,
  c.prev_state,
  c.prev_state_duration,
  coalesce(r.rank, 1000) AS state_rank,
  c.state_change_reason,
  iif(c.state = 'NONEXISTENT', fw.hosting_type, NULL) AS hosting_type,
  iif(c.state = 'NONEXISTENT', trim(fw.hosting_name, '{}'), NULL) AS hosting_name,
  iif(c.state = 'NONEXISTENT', fw.trigger_type, NULL) AS trigger_type,
  iif(c.state = 'NONEXISTENT', fw.bind_application_delay_ms, NULL) AS bind_application_delay_ms,
  iif(c.state = 'NONEXISTENT', fw.process_start_delay_ms, NULL) AS process_start_delay_ms,
  iif(c.state = 'EXITED', fw.exit_reason, NULL) AS exit_reason,
  iif(c.state = 'EXITED', fw.exit_sub_reason, NULL) AS exit_subreason
FROM state_changes AS c
JOIN process AS p USING (upid)
LEFT JOIN android_process_metadata AS m USING (upid)
LEFT JOIN _android_process_state_rank AS r
  ON r.state = c.state
LEFT JOIN __intrinsic_android_track_event_process AS fw USING (upid);

-- Number of processes concurrently in each framework process state over time.
CREATE PERFETTO TABLE _android_process_state_concurrency AS
SELECT
  row_number() OVER (ORDER BY c.group_name, c.ts) AS id,
  c.ts,
  coalesce(
    lead(c.ts) OVER (PARTITION BY c.group_name ORDER BY c.ts) - c.ts,
    max(trace_end() - c.ts, 0)
  ) AS dur,
  c.group_name AS state,
  coalesce(r.rank, 1000) AS state_rank,
  c.value AS concurrency
FROM intervals_overlap_count_by_group!(
  _android_process_state_intervals, ts, dur, state
) AS c
LEFT JOIN _android_process_state_rank AS r
  ON r.state = c.group_name;
