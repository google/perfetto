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

-- NOTE (psqlnext): the `intervals.intersect` and `intervals.overlap` modules are
-- DELETED. `_interval_intersect!` is `INTERVAL INTERSECTION OF (… PER cpu)`,
-- `_intervals_flatten!` is `INTERVAL FLATTEN AGGREGATE ARG_MAX(depth, …)`, and the
-- partitioned `SPAN_OUTER_JOIN`s are `INTERVAL UNION OF (… PER cpu)`.

INCLUDE PERFETTO MODULE android.process_metadata;

INCLUDE PERFETTO MODULE linux.irqs;

INCLUDE PERFETTO MODULE wattson.cpu.idle;

INCLUDE PERFETTO MODULE wattson.utils;

-- Get slices only where there is transition from deep idle to active. A stable
-- per-row id identifies each idle exit (the old `_auto_id`-keyed group).
CREATE PERFETTO PIPELINE _idle_exits MATERIALIZED AS
FROM _adjusted_deep_idle
|> WHERE idle = -1
|> SELECT ROW_NUMBER() OVER (ORDER BY ts) AS group_id, ts, dur, cpu, idle;

-- Establish relationships between tasks, such as thread/process/package
CREATE PERFETTO PIPELINE _task_wo_irq_infos MATERIALIZED AS
FROM thread
|> JOIN sched USING (utid)
|> LEFT JOIN process USING (upid)
|> LEFT JOIN android_process_metadata AS package USING (upid)
-- Some slices have -1 duration when there is no end (e.g. slices at the end of a
-- trace), so need this check to exclude negative dur slices.
|> WHERE sched.dur > 0
|> SELECT
     sched.ts,
     sched.dur,
     sched.cpu,
     thread.utid,
     thread.upid,
     thread.tid,
     process.pid,
     package.uid,
     thread.name AS thread_name,
     process.name AS process_name,
     package.package_name;

-- Flatten hard IRQs, since they can preempt each other. Only hard IRQs can use
-- the built-in ancestor_slice() and interval functions.
CREATE PERFETTO PIPELINE _hard_irq_flattened_slices AS
FROM linux_hard_irqs AS irq
|> SELECT irq.id, irq.ts, irq.dur
-- Resolve self-overlap created by preempting hard IRQs into disjoint segments,
-- keeping the deepest (innermost) IRQ live in each segment.
|> INTERVAL FLATTEN AGGREGATE ARG_MAX(irq.ts, irq.id) AS id
|> JOIN slices USING (id)
|> JOIN cpu_track ON cpu_track.id = slices.track_id
|> SELECT ts, dur, slices.name AS hard_irq_name, cpu_track.cpu;

-- Softirqs run with other softirqs disabled, so will not be preempted by each
-- other, and thus do not need to be flattened like hard IRQs do.
CREATE PERFETTO PIPELINE _soft_irq_slices AS
FROM linux_soft_irqs
|> JOIN slices USING (id)
|> JOIN cpu_track ON cpu_track.id = slices.track_id
|> SELECT
     linux_soft_irqs.ts,
     linux_soft_irqs.dur,
     slices.name AS soft_irq_name,
     cpu_track.cpu;

-- Replace soft IRQs with hard IRQs if hard IRQs are present. Hard IRQs could
-- preempt soft IRQs, but not the other way around.
CREATE PERFETTO PIPELINE _all_irqs_flattened_slices AS
INTERVAL UNION OF (
  _soft_irq_slices AS soft,
  _hard_irq_flattened_slices AS hard
) PER cpu
|> SELECT
     ts,
     dur,
     cpu,
     coalesce(hard.hard_irq_name, soft.soft_irq_name) AS irq_name,
     -- Create a synthetic irq_id such that IRQ slices have the same
     -- properties/columns as thread slices, which allows us to fit IRQ slices
     -- into the existing framework of attributing power to tasks.
     hash(coalesce(hard.hard_irq_name, soft.soft_irq_name)) AS irq_id;

-- Replace nominal tasks with IRQ if the IRQ slice is present. IRQs could
-- preempt tasks, but not the other way around.
CREATE PERFETTO PIPELINE _all_tasks_flattened_slices MATERIALIZED AS
INTERVAL UNION OF (
  _task_wo_irq_infos AS task,
  _all_irqs_flattened_slices AS irq
) PER cpu
|> SELECT
     ts,
     dur,
     cpu,
     coalesce(irq.irq_id, task.utid) AS utid,
     coalesce(irq.irq_id, task.upid) AS upid,
     coalesce(irq.irq_id, task.tid) AS tid,
     coalesce(irq.irq_id, task.pid) AS pid,
     coalesce(irq.irq_id, task.uid) AS uid,
     coalesce(irq.irq_name, task.thread_name) AS thread_name,
     coalesce(irq.irq_name, task.process_name) AS process_name,
     coalesce(irq.irq_name, task.package_name) AS package_name,
     NOT (irq.irq_id IS NULL) AS is_irq;

-- Associate idle states, and specifically the active state, with tasks.
CREATE PERFETTO PIPELINE _active_state_w_tasks MATERIALIZED AS
INTERVAL INTERSECTION OF (
  _all_tasks_flattened_slices AS tasks,
  _idle_exits AS idle
) PER cpu
|> SELECT
     ts,
     dur,
     cpu,
     tasks.utid,
     tasks.upid,
     tasks.tid,
     tasks.pid,
     tasks.uid,
     tasks.thread_name,
     tasks.process_name,
     tasks.package_name,
     tasks.is_irq,
     idle.group_id AS idle_group;

-- Find the task responsible for causing the idle exit, and remove all tasks
-- before it (effectively only IRQs and swappers). This logic creates a table
-- wherein the first task in the table is the one that caused the idle exit.
CREATE PERFETTO PIPELINE _task_causing_idle_exit MATERIALIZED AS
FROM _active_state_w_tasks
|> EXTEND
     -- If there are non-IRQs in this idle_group, select the first non-IRQ task
     -- as the first row. Otherwise, select the first IRQ as the first row.
     row_number() OVER (
       PARTITION BY idle_group
       ORDER BY (CASE WHEN NOT is_irq AND utid > 0 THEN 0 ELSE 1 END), ts
     ) AS rn
|> WHERE rn = 1
|> SELECT ts AS boundary_ts, idle_group;

--- Recreate all known tasks in the context of power estimation, meaning that
--- tasks (usually IRQs) that do not contribute to power attribution are removed
--- and replaced with swapper. The previous table, _active_state_w_tasks, has
--- many groups of "islands", of which the gaps need to be filled back in with
--- the swapper task.
CREATE PERFETTO PIPELINE _wattson_task_slices MATERIALIZED AS
SUBPIPELINE base_tasks AS (
  FROM _active_state_w_tasks AS t
  |> JOIN _task_causing_idle_exit AS exit USING (idle_group)
  |> WHERE t.ts >= exit.boundary_ts
  |> SELECT
       t.ts, t.dur, t.cpu, t.idle_group, t.utid, t.upid, t.tid, t.pid, t.uid,
       t.thread_name, t.process_name, t.package_name
)
SUBPIPELINE swapper_gaps AS (
  FROM base_tasks
  |> AGGREGATE
       min(ts) AS island_start,
       max(ts + dur) AS island_end
     GROUP BY cpu, idle_group
  |> SELECT
       island_end AS ts,
       lead(island_start) OVER (PARTITION BY cpu ORDER BY island_start)
         - island_end AS dur,
       cpu
)
-- Combine the real tasks with the calculated swapper gaps.
FROM base_tasks
|> SELECT
     ts, dur, cpu, utid, upid, tid, pid, uid,
     thread_name, process_name, package_name
|> UNION ALL (
     FROM swapper_gaps
     |> WHERE dur > 0
     |> SELECT
          ts,
          dur,
          cpu,
          0 AS utid,
          0 AS upid,
          0 AS tid,
          0 AS pid,
          NULL AS uid,
          'swapper' AS thread_name,
          NULL AS process_name,
          NULL AS package_name
   );
