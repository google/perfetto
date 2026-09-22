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
        'PROCESS_STATE_UNSPECIFIED',
        997
      ), ('PROCESS_STATE_UNKNOWN_TO_PROTO', 998), ('PROCESS_STATE_UNKNOWN', 999)
  )
SELECT replace(state, 'PROCESS_STATE_', '') AS state, rank FROM r;

-- Intervals of Android framework process states.
-- One interval per state a process was in.
CREATE PERFETTO TABLE _android_process_state_intervals AS
WITH
  process_lifetimes AS (
    SELECT
      p.upid,
      coalesce(fw.fw_start_ts, p.start_ts, trace_start()) AS start_ts,
      coalesce(fw.fw_end_ts, p.end_ts, trace_end()) AS end_ts
    FROM process AS p
    LEFT JOIN __intrinsic_android_track_event_process AS fw USING (upid)
  ),
  raw_changes AS (
    SELECT
      coalesce(s.ts, p.start_ts) AS ts,
      s.is_initial,
      s.upid,
      replace(s.proc_state, 'PROCESS_STATE_', '') AS cur_state,
      lag(replace(s.proc_state, 'PROCESS_STATE_', '')) OVER (
        PARTITION BY
          s.upid
        ORDER BY coalesce(s.ts, p.start_ts), s.is_initial DESC
      ) AS prev_state,
      s.reason AS cur_reason,
      p.end_ts
    FROM __intrinsic_android_process_state AS s
    JOIN process_lifetimes AS p USING (upid)
  ),
  state_changes AS (
    SELECT
      c.ts,
      max(
        coalesce(
          lead(c.ts) OVER (PARTITION BY c.upid ORDER BY c.ts, c.is_initial DESC),
          c.end_ts
        )
        - c.ts,
        0
      ) AS dur,
      c.upid,
      c.cur_state AS state,
      c.prev_state,
      c.ts
      - lag(c.ts) OVER (PARTITION BY c.upid ORDER BY c.ts, c.is_initial DESC) AS prev_state_duration,
      c.cur_reason AS reason
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
  c.reason
FROM state_changes AS c
JOIN process AS p USING (upid)
LEFT JOIN android_process_metadata AS m USING (upid)
LEFT JOIN _android_process_state_rank AS r
  ON r.state = c.state
WHERE
  c.state != 'NONEXISTENT';

-- Number of processes concurrently in each framework process state over time.
CREATE PERFETTO TABLE _android_process_state_concurrency AS
SELECT
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
