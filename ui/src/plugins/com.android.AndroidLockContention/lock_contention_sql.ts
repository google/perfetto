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

// TODO(ivankc) Consider moving this to stdlib
export const LOCK_CONTENTION_SQL = `
INCLUDE PERFETTO MODULE intervals.intersect;
INCLUDE PERFETTO MODULE android.monitor_contention;
INCLUDE PERFETTO MODULE slices.with_context;

-- Contains parsed lock contention events, including the owner tid and blocked/blocking thread names.
CREATE OR REPLACE PERFETTO VIEW android_lock_contention AS
WITH raw_contentions AS (
  SELECT
    s.id,
    s.ts,
    s.dur,
    s.name,
    s.utid,
    cast_int!(STR_SPLIT(STR_SPLIT(s.name, '(owner tid: ', 1), ')', 0)) AS owner_tid
  FROM thread_slice AS s
  WHERE
    s.name GLOB 'Lock contention*(owner tid: *)*'
)
SELECT
  r.id,
  r.ts,
  r.dur,
  r.name,
  r.owner_tid,
  obt.utid AS owner_utid,
  bt.name AS blocked_thread_name,
  obt.name AS blocking_thread_name,
  regexp_extract(r.name, 'Lock contention on (?:a )?(.*) lock') AS lock_type
FROM raw_contentions AS r
JOIN thread AS bt
  ON r.utid = bt.utid
LEFT JOIN thread AS obt
  ON obt.tid = r.owner_tid
  AND (obt.upid = bt.upid OR r.owner_tid = 0);

-- Contains the union of all lock contention events from both ART and Monitor contention sources.
CREATE OR REPLACE PERFETTO TABLE android_all_lock_contentions AS
SELECT
  id,
  ts,
  dur,
  name AS lock_name,
  owner_tid,
  owner_utid,
  blocked_thread_name,
  blocking_thread_name,
  0 AS is_monitor,
  lock_type
FROM android_lock_contention
WHERE owner_utid IS NOT NULL AND dur > 0
UNION ALL
SELECT
  id,
  ts,
  dur,
  lock_name,
  blocking_tid AS owner_tid,
  blocking_utid AS owner_utid,
  blocked_thread_name,
  blocking_thread_name,
  1 AS is_monitor,
  NULL AS lock_type
FROM android_monitor_contention
WHERE blocking_utid IS NOT NULL AND dur > 0;

-- Contains events for the custom tracks, including depth for layout.
CREATE OR REPLACE PERFETTO TABLE __android_lock_contention_owner_events AS
WITH unique_events AS (
  SELECT DISTINCT
    id,
    owner_tid,
    ts,
    dur,
    lock_name,
    lock_type,
    blocked_thread_name,
    blocking_thread_name,
    CASE
      WHEN is_monitor THEN 'Blocking ' || IFNULL(blocked_thread_name, 'unknown thread') || ' on ' || 
        CASE WHEN lock_name = 'Unknown Lock' OR lock_name IS NULL THEN '' ELSE lock_name || ' ' END || 'monitor lock'
      ELSE 'Blocking ' || IFNULL(blocked_thread_name, 'unknown thread') || ' on ' || COALESCE(lock_type || ' ', '') || 'lock'
    END AS name
  FROM android_all_lock_contentions
)
SELECT 
  id,
  owner_tid,
  ts,
  dur,
  lock_name,
  lock_type,
  blocked_thread_name,
  blocking_thread_name,
  name,
  internal_layout(ts, dur) OVER (PARTITION BY owner_tid ORDER BY ts ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS depth
FROM unique_events;

-- Extract all unique utids that act as lock owners
CREATE OR REPLACE PERFETTO TABLE _all_lock_blocking_utids AS
SELECT DISTINCT owner_utid AS utid
FROM android_all_lock_contentions
WHERE owner_utid IS NOT NULL;

-- Filter thread_state to only include these utids
CREATE OR REPLACE PERFETTO VIEW _all_lock_blocking_thread_state AS
SELECT
  id,
  utid AS owner_utid,
  ts,
  dur,
  state,
  blocked_function
FROM thread_state
WHERE utid IN (SELECT utid FROM _all_lock_blocking_utids)
  AND dur >= 0;

-- Filter contentions to only include valid durations for interval intersect
CREATE OR REPLACE PERFETTO VIEW _android_all_lock_contentions_valid_dur AS
SELECT * FROM android_all_lock_contentions
WHERE dur >= 0;
-- Use interval intersect to get thread states during all lock contentions
CREATE OR REPLACE PERFETTO VIEW _android_all_lock_contention_thread_state_intersect AS
SELECT * FROM _interval_intersect_with_col_names!(
  _android_all_lock_contentions_valid_dur, id, ts, dur,
  _all_lock_blocking_thread_state, id, ts, dur,
  (owner_utid)
);

-- Unified view of thread states for all lock contentions
CREATE OR REPLACE PERFETTO VIEW android_all_lock_contention_thread_state AS
SELECT
  ii.id_0 AS id,
  ii.ts,
  ii.dur,
  ii.owner_utid,
  bts.blocked_function,
  bts.state
FROM _android_all_lock_contention_thread_state_intersect ii
JOIN _all_lock_blocking_thread_state bts ON ii.id_1 = bts.id;

-- Aggregated thread_states for all lock contentions
CREATE OR REPLACE PERFETTO VIEW android_all_lock_contention_thread_state_by_txn AS
SELECT
  id,
  state AS thread_state,
  SUM(dur) AS thread_state_dur,
  COUNT(1) AS thread_state_count
FROM android_all_lock_contention_thread_state
GROUP BY id, state;

-- Aggregated blocked_functions for all lock contentions
CREATE OR REPLACE PERFETTO VIEW android_all_lock_contention_blocked_functions_by_txn AS
SELECT
  id,
  blocked_function,
  SUM(dur) AS blocked_function_dur,
  COUNT(1) AS blocked_function_count
FROM android_all_lock_contention_thread_state
WHERE blocked_function IS NOT NULL
GROUP BY id, blocked_function;
`;
