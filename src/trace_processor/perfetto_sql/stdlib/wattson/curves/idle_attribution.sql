--
-- Copyright 2024 The Android Open Source Project
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

INCLUDE PERFETTO MODULE intervals.intersect;
INCLUDE PERFETTO MODULE wattson.curves.grouped;

-- Get slice info of threads/processes
CREATE PERFETTO TABLE _thread_process_slices AS
SELECT
  sched.ts,
  sched.dur,
  sched.cpu,
  thread.utid,
  thread.upid
FROM thread
JOIN sched USING (utid)
WHERE dur > 0;

-- Helper macro so Perfetto tables can be used with interval intersect
CREATE PERFETTO MACRO _ii_table(tab TableOrSubquery)
RETURNS TableOrSubquery AS (SELECT _auto_id AS id, * FROM $tab);

-- Get slices only where there is transition from deep idle to active
CREATE PERFETTO TABLE _idle_exits AS
SELECT
  ts,
  dur,
  cpu,
  idle
FROM _adjusted_deep_idle
WHERE idle = -1 and dur > 0;

-- Gets the slices where the CPU transitions from deep idle to active, and the
-- associated thread that causes the idle exit
CREATE PERFETTO TABLE _idle_w_threads AS
WITH _ii_idle_threads AS (
  SELECT
    ii.ts,
    ii.dur,
    ii.cpu,
    threads.utid,
    threads.upid,
    id_1 as idle_group
  FROM _interval_intersect!(
    (
      _ii_table!(_thread_process_slices),
      _ii_table!(_idle_exits)
    ),
    (cpu)
  ) ii
  JOIN _thread_process_slices AS threads
    ON threads._auto_id = id_0
),
-- Since sorted by time, MIN() is fast aggregate function that will return the
-- first time slice, which will be the utid = 0 slice immediately succeeding the
-- idle to active transition, and immediately preceding the active thread
first_swapper_slice AS (
  SELECT
    ts,
    dur,
    cpu,
    idle_group,
    MIN(ts) as min
  FROM _ii_idle_threads
  GROUP BY idle_group
),
-- MIN() here will give the first active thread immediately succeeding the idle
-- to active transition slice, which means this the the thread that causes the
-- idle exit
first_non_swapper_slice AS (
  SELECT
    idle_group,
    utid,
    upid,
    MIN(ts) as min
  FROM _ii_idle_threads
  WHERE utid != 0
  GROUP BY idle_group
)
SELECT
  ts,
  dur,
  cpu,
  utid,
  upid
FROM first_non_swapper_slice
JOIN first_swapper_slice USING (idle_group);

-- Interval intersect with the estimate power track, so that each slice can be
-- attributed to the power of the CPU in that time duration
CREATE PERFETTO TABLE _idle_transition_cost AS
SELECT
  ii.ts,
  ii.dur,
  threads.cpu,
  threads.utid,
  threads.upid,
  CASE threads.cpu
    WHEN 0 THEN power.cpu0_mw
    WHEN 1 THEN power.cpu1_mw
    WHEN 2 THEN power.cpu2_mw
    WHEN 3 THEN power.cpu3_mw
    WHEN 4 THEN power.cpu4_mw
    WHEN 5 THEN power.cpu5_mw
    WHEN 6 THEN power.cpu6_mw
    WHEN 7 THEN power.cpu7_mw
    ELSE 0
  END estimated_mw
FROM _interval_intersect!(
  (
    _ii_table!(_idle_w_threads),
    _ii_table!(_system_state_mw)
  ),
  ()
) ii
JOIN _idle_w_threads as threads ON threads._auto_id = id_0
JOIN _system_state_mw as power ON power._auto_id = id_1;

-- Macro for easily filtering idle attribution to a specified time window. This
-- information can then further be filtered by specific CPU and GROUP BY on
-- either utid or upid
CREATE PERFETTO FUNCTION _filter_idle_attribution(ts LONG, dur LONG)
RETURNS Table(idle_cost_mws LONG, utid INT, upid INT, cpu INT) AS
SELECT
  cost.estimated_mw * cost.dur / 1e9 as idle_cost_mws,
  cost.utid,
  cost.upid,
  cost.cpu
FROM _interval_intersect_single!(
  $ts, $dur, _ii_table!(_idle_transition_cost)
) ii
JOIN _idle_transition_cost as cost ON cost._auto_id = id;
