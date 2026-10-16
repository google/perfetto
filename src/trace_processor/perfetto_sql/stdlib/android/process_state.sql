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

INCLUDE PERFETTO MODULE intervals.fill_gaps;

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
  process_lifetimes AS (
    SELECT
      p.upid,
      -- AndroidProcessStartEvent only sets fw_start_ts; process.start_ts comes
      -- from ftrace forks.
      coalesce(fw.fw_start_ts, p.start_ts, trace_start()) AS start_ts,
      p.end_ts AS death_ts
    FROM (SELECT DISTINCT upid FROM __intrinsic_android_process_state)
    JOIN process AS p USING (upid)
    LEFT JOIN __intrinsic_android_track_event_process AS fw USING (upid)
  ),
  all_events AS (
    SELECT
      coalesce(s.ts, p.start_ts) AS ts,
      s.is_initial,
      s.upid,
      replace(s.proc_state, 'PROCESS_STATE_', '') AS cur_state,
      s.reason AS cur_reason
    FROM __intrinsic_android_process_state AS s
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

-- Freezer state of each process over its lifetime, from the framework's
-- freezer events. A freeze event carries unfrozen_dur_ms and an unfreeze event
-- frozen_dur_ms, which is what tells them apart. Processes with no freezer
-- event in the trace only appear in the trace-stop snapshot, which carries an
-- unfreeze_reason for unfrozen processes only.
CREATE PERFETTO TABLE _android_process_freezer_intervals AS
WITH
  events AS (
    SELECT
      upid,
      ts,
      iif(frozen_dur_ms IS NOT NULL, 'UNFROZEN', 'FROZEN') AS state,
      lead(ts) OVER (PARTITION BY upid ORDER BY ts) AS next_ts,
      row_number() OVER (PARTITION BY upid ORDER BY ts) AS n
    FROM __intrinsic_android_freezer_state
    WHERE
      is_initial = 0
  ),
  -- A frozen process that gets killed emits no unfreeze, so clip to the
  -- lifetime seen by the state stream.
  lifetimes AS (
    SELECT
      upid,
      max(min(iif(state = 'NONEXISTENT', ts + dur, ts)), trace_start()) AS birth,
      coalesce(min(iif(state = 'EXITED', ts, NULL)), trace_end()) AS death
    FROM _android_process_state_intervals
    GROUP BY
      upid
  ),
  spans AS (
    SELECT
      e.upid,
      e.ts,
      min(coalesce(e.next_ts, trace_end()), l.death) - e.ts AS dur,
      e.state
    FROM events AS e
    JOIN lifetimes AS l USING (upid)
    UNION ALL
    -- Before its first event a process was in the opposite state.
    SELECT
      e.upid,
      l.birth AS ts,
      e.ts - l.birth AS dur,
      iif(e.state = 'FROZEN', 'UNFROZEN', 'FROZEN') AS state
    FROM events AS e
    JOIN lifetimes AS l USING (upid)
    WHERE
      e.n = 1
    UNION ALL
    -- Processes without events kept their trace-stop state throughout.
    SELECT
      s.upid,
      l.birth AS ts,
      l.death - l.birth AS dur,
      iif(s.unfreeze_reason IS NULL, 'FROZEN', 'UNFROZEN') AS state
    FROM __intrinsic_android_freezer_state AS s
    JOIN lifetimes AS l USING (upid)
    WHERE
      s.is_initial = 1
      AND NOT EXISTS (SELECT 1 FROM events AS e WHERE e.upid = s.upid)
  )
SELECT
  row_number() OVER (ORDER BY s.ts, s.upid) AS id,
  s.ts,
  s.dur,
  s.upid,
  p.pid,
  m.process_name,
  s.state
FROM spans AS s
JOIN process AS p USING (upid)
LEFT JOIN android_process_metadata AS m USING (upid)
WHERE
  s.dur > 0;

-- A pipeline cannot number its rows, hence the intermediate table.
CREATE PERFETTO TABLE _android_process_freezer_flattened AS
FROM _android_process_freezer_intervals
|> INTERVAL FLATTEN PER state AGGREGATE COUNT(*) AS concurrency;

-- Number of processes concurrently frozen and unfrozen over time. Flattening
-- skips the stretches with no process in a state, so fill them with zeros to
-- make the counters drop to 0 there.
CREATE PERFETTO TABLE _android_process_freezer_concurrency AS
SELECT
  row_number() OVER (ORDER BY state, ts) AS id,
  ts,
  dur,
  state,
  coalesce(concurrency, 0) AS concurrency
FROM _intervals_fill_gaps!(
  (state),
  (concurrency),
  _android_process_freezer_flattened
)
WHERE
  state IS NOT NULL;
