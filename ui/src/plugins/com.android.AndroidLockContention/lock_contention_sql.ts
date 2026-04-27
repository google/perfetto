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
  obt.name AS blocking_thread_name
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
  blocking_thread_name
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
  blocking_thread_name
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
    blocked_thread_name,
    blocking_thread_name,
    '[Lock Owner] Blocking' AS name
  FROM android_all_lock_contentions
)
SELECT 
  id,
  owner_tid,
  ts,
  dur,
  lock_name,
  blocked_thread_name,
  blocking_thread_name,
  name,
  internal_layout(ts, dur) OVER (PARTITION BY owner_tid ORDER BY ts ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS depth
FROM unique_events;
`;
