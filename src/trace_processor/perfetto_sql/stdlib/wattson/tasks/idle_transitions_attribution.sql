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

INCLUDE PERFETTO MODULE wattson.estimates;

INCLUDE PERFETTO MODULE wattson.tasks.task_slices;

INCLUDE PERFETTO MODULE wattson.utils;

-- Gets the slices where the CPU transitions from deep idle to active, and the
-- associated task that causes the idle exit
CREATE PERFETTO PIPELINE _idle_w_tasks MATERIALIZED AS
-- Co-fragment task slices against idle-exit intervals per cpu; `idle.group_id`
-- identifies the idle group each fragment falls in.
SUBPIPELINE ii_idle_tasks AS (
  INTERVAL INTERSECTION OF (
    _wattson_task_slices AS tasks,
    _idle_exits AS idle
  ) PER cpu
  |> SELECT
       ts,
       dur,
       cpu,
       tasks.utid,
       tasks.upid,
       tasks.uid,
       idle.group_id AS idle_group
  |> ORDER BY ts
)
-- Since sorted by time, MIN() is fast aggregate function that will return the
-- first time slice, which will be the utid = 0 slice immediately succeeding the
-- idle to active transition, and immediately preceding the active task
SUBPIPELINE first_swapper_slice AS (
  FROM ii_idle_tasks
  |> WHERE utid IN (SELECT utid FROM thread WHERE is_idle)
  |> AGGREGATE ARG_MIN(ts, ts) AS ts, ARG_MIN(ts, dur) AS dur, ARG_MIN(ts, cpu) AS cpu, min(ts) AS min GROUP BY idle_group
)
-- MIN() here will give the first active task immediately succeeding the idle
-- to active transition slice, which means this the the task that causes the
-- idle exit
SUBPIPELINE first_non_swapper_slice AS (
  FROM ii_idle_tasks
  |> WHERE NOT (utid IN (SELECT utid FROM thread WHERE is_idle))
  |> AGGREGATE
       ARG_MIN(ts, utid) AS utid, ARG_MIN(ts, upid) AS upid, ARG_MIN(ts, uid) AS uid, min(ts) AS min, min(ts) + ARG_MIN(ts, dur) AS next_ts
     GROUP BY idle_group
)
-- MAX() here will give the last time slice in the group. This will be the
-- utid = 0 slice immediately preceding the active to idle transition.
SUBPIPELINE last_swapper_slice AS (
  FROM ii_idle_tasks
  |> WHERE utid IN (SELECT utid FROM thread WHERE is_idle)
  |> AGGREGATE ARG_MAX(ts, ts) AS ts, ARG_MAX(ts, dur) AS dur, ARG_MAX(ts, cpu) AS cpu, max(ts) AS min GROUP BY idle_group
)
FROM first_non_swapper_slice AS task_info
|> JOIN first_swapper_slice AS swapper_info USING (idle_group)
|> SELECT
     swapper_info.ts,
     swapper_info.dur,
     swapper_info.cpu,
     task_info.utid,
     task_info.upid,
     task_info.uid
|> UNION ALL (
     -- Adds the last slice to idle transition attribution IF this is a singleton
     -- task wakeup. This is true if there is only one task between swapper idle
     -- exits/wakeups. For example, groups with order of swapper, task X, swapper
     -- will be included. Entries that have multiple task between swappers, such as
     -- swapper, task X, task Y, swapper will not be included.
     FROM first_non_swapper_slice AS task_info
     |> JOIN last_swapper_slice AS swapper_info USING (idle_group)
     |> WHERE swapper_info.ts = task_info.next_ts
     |> SELECT
          swapper_info.ts,
          swapper_info.dur,
          swapper_info.cpu,
          task_info.utid,
          task_info.upid,
          task_info.uid
   );

-- Interval intersect with the estimate power track, so that each slice can be
-- attributed to the power of the CPU in that time duration
CREATE PERFETTO PIPELINE _idle_transition_cost MATERIALIZED AS
INTERVAL INTERSECTION OF (
  _idle_w_tasks AS tasks,
  _system_state_mw AS power
)
|> SELECT
     ts,
     dur,
     tasks.cpu,
     tasks.utid,
     tasks.upid,
     tasks.uid,
     CASE tasks.cpu
       WHEN 0 THEN power.cpu0_mw
       WHEN 1 THEN power.cpu1_mw
       WHEN 2 THEN power.cpu2_mw
       WHEN 3 THEN power.cpu3_mw
       WHEN 4 THEN power.cpu4_mw
       WHEN 5 THEN power.cpu5_mw
       WHEN 6 THEN power.cpu6_mw
       WHEN 7 THEN power.cpu7_mw
       ELSE 0
     END AS estimated_mw;

-- Filters idle attribution to a specified time window. This information can then
-- further be filtered by specific CPU and GROUP BY on either utid or upid.
-- (A pipeline-valued macro: clipping to a caller-supplied window is
-- `_interval_intersect_single!`.)
CREATE PERFETTO MACRO _filter_idle_attribution(ts Expr, dur Expr)
RETURNS Pipeline AS (
  -- Each idle-transition cost clipped to [$ts, $ts + $dur); the clip sets `dur`
  -- to the in-window overlap, so the energy rescales by the clipped duration.
  _interval_intersect_single!($ts, $dur, _idle_transition_cost)
  |> SELECT estimated_mw * dur / 1e9 AS idle_cost_mws, utid, upid, uid, cpu
  |> FORK AS base
  |> UNION ALL (
       -- Give the negative sum of idle costs to the swapper thread (utid/upid 0),
       -- which by definition is otherwise undefined.
       FROM base
       |> AGGREGATE -1 * sum(idle_cost_mws) AS idle_cost_mws GROUP BY cpu
       |> SELECT idle_cost_mws, 0 AS utid, 0 AS upid, 0 AS uid, cpu
     );
