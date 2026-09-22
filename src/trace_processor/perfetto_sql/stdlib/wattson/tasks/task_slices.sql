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

INCLUDE PERFETTO MODULE android.process_metadata;

INCLUDE PERFETTO MODULE intervals.intersect;

INCLUDE PERFETTO MODULE intervals.overlap;

INCLUDE PERFETTO MODULE linux.irqs;

INCLUDE PERFETTO MODULE wattson.cpu.idle;

INCLUDE PERFETTO MODULE wattson.utils;

-- Get slices only where there is transition from deep idle to active
CREATE PERFETTO TABLE _idle_exits AS
SELECT ts, dur, cpu, idle, _auto_id AS group_id
FROM _adjusted_deep_idle
WHERE
  idle = -1;

-- Slices where tasks ran without IRQ information
CREATE PERFETTO TABLE _task_wo_irq_infos AS
SELECT ts, dur, cpu, utid
FROM sched
WHERE
  -- Some slices have -1 duration when there is no end (e.g. slices at the end
  -- of a trace), so need this check to exclude negative dur slices.
  dur > 0;

-- Flatten hard IRQs, since they can preempt each other. Only hard IRQs can use
-- the built-in ancestor_slice() and interval functions
CREATE PERFETTO VIEW _hard_irq_flattened_slices AS
WITH
  root_slices AS (
    SELECT id, ts, dur FROM linux_hard_irqs WHERE parent_id IS NULL
  ),
  child_slices AS (
    SELECT anc.id AS root_id, irq.id, irq.parent_id, irq.ts, irq.dur
    FROM linux_hard_irqs AS irq, ancestor_slice(irq.id) AS anc
    WHERE
      irq.parent_id IS NOT NULL
  )
SELECT intervals.ts, intervals.dur, slices.name AS hard_irq_name, cpu_track.cpu
FROM _intervals_flatten!(_intervals_merge_root_and_children!(root_slices, child_slices)) AS intervals
JOIN slices USING (id)
JOIN cpu_track
  ON cpu_track.id = slices.track_id;

-- Softirqs run with other softirqs disabled, so will not be preempted by each
-- other, and thus do not need to be flattened like hard IRQs do.
CREATE PERFETTO VIEW _soft_irq_slices AS
SELECT
  linux_soft_irqs.ts,
  linux_soft_irqs.dur,
  slices.name AS soft_irq_name,
  cpu_track.cpu
FROM linux_soft_irqs
JOIN slices USING (id)
JOIN cpu_track
  ON cpu_track.id = slices.track_id;

CREATE VIRTUAL TABLE _all_irqs_combined_slices USING SPAN_OUTER_JOIN(
  _soft_irq_slices PARTITIONED cpu,
  _hard_irq_flattened_slices PARTITIONED cpu
);

-- Replace soft IRQs with hard IRQs if hard IRQs are present. Hard IRQs could
-- preempt soft IRQs, but not the other way around.
CREATE PERFETTO VIEW _all_irqs_flattened_slices AS
SELECT
  ts,
  dur,
  cpu,
  coalesce(hard_irq_name, soft_irq_name) AS irq_name,
  -- Create a synthetic irq_id such that IRQ slices have the same
  -- properties/columns as thread slices, which allows us to fit IRQ slices into
  -- the existing framework of attributing power to tasks.
  hash(coalesce(hard_irq_name, soft_irq_name)) AS irq_id
FROM _all_irqs_combined_slices;

-- Thread/process/package descriptions of every task, keyed by utid.
--
-- Splitting metadata out from task slices avoids dragging 7 descriptive columns
-- through every slice in task_slices.sql and _estimates_w_tasks_attribution.
CREATE PERFETTO TABLE _wattson_task_metadata AS
SELECT
  0 AS utid,
  0 AS upid,
  0 AS tid,
  0 AS pid,
  NULL AS uid,
  'swapper' AS thread_name,
  NULL AS process_name,
  NULL AS package_name
UNION ALL
SELECT
  thread.utid,
  thread.upid,
  thread.tid,
  process.pid,
  package.uid,
  thread.name AS thread_name,
  process.name AS process_name,
  package.package_name
FROM thread
LEFT JOIN process USING (upid)
LEFT JOIN android_process_metadata AS package USING (upid)
WHERE
  thread.utid != 0
UNION ALL
SELECT DISTINCT
  irq_id AS utid,
  irq_id AS upid,
  irq_id AS tid,
  irq_id AS pid,
  irq_id AS uid,
  irq_name AS thread_name,
  irq_name AS process_name,
  irq_name AS package_name
FROM _all_irqs_flattened_slices;

CREATE PERFETTO INDEX _wattson_task_metadata_idx ON _wattson_task_metadata(utid);

-- SPAN_OUTER_JOIN needed because IRQ table do not have contiguous slices,
-- whereas tasks table will be contiguous
CREATE VIRTUAL TABLE _irq_w_tasks_info USING SPAN_OUTER_JOIN(
  _task_wo_irq_infos PARTITIONED cpu,
  _all_irqs_flattened_slices PARTITIONED cpu
);

-- Replace nominal tasks with IRQ if the IRQ slice is present. IRQs could
-- preempt tasks, but not the other way around.
CREATE PERFETTO TABLE _all_tasks_flattened_slices AS
SELECT
  ts,
  dur,
  cpu,
  coalesce(irq_id, utid) AS utid,
  NOT (irq_id IS NULL) AS is_irq
FROM _irq_w_tasks_info;

-- Associate idle states, and specifically the active state, with tasks
CREATE PERFETTO TABLE _active_state_w_tasks AS
SELECT ii.ts, ii.dur, ii.cpu, tasks.utid, tasks.is_irq, id_1 AS idle_group
FROM _interval_intersect!(
(
  _ii_subquery!(_all_tasks_flattened_slices),
  _ii_subquery!(_idle_exits)
),
(cpu)
) AS ii
JOIN _all_tasks_flattened_slices AS tasks
  ON tasks._auto_id = id_0;

CREATE PERFETTO INDEX _active_state_w_tasks_group ON _active_state_w_tasks(
  idle_group,
  ts
);

-- Find the task responsible for causing the idle exit, and remove all tasks
-- before it (effectively only IRQs and swappers). This logic creates a table
-- wherein the first task in the table is the one that caused the idle exit.
CREATE PERFETTO TABLE _task_causing_idle_exit AS
SELECT
  -- Prefer the earliest real (non-IRQ, non-swapper) task in the group; if the
  -- group has none, fall back to the earliest row of any kind.
  coalesce(min(iif(NOT is_irq AND utid > 0, ts, NULL)), min(ts)) AS boundary_ts,
  idle_group
FROM _active_state_w_tasks
GROUP BY
  idle_group;

CREATE PERFETTO INDEX _task_causing_idle_exit_idx ON _task_causing_idle_exit(
  idle_group,
  boundary_ts
);

--- Recreate all known tasks in the context of power estimation, meaning that
--- tasks (usually IRQs) that do not contribute to power attribution are removed
--- and replaced with swapper. The previous table, _active_state_w_tasks, has
--- many groups of "islands", of which the gaps need to be filled back in with
--- the swapper task.
CREATE PERFETTO TABLE _wattson_task_slices AS
WITH
  base_tasks AS (
    SELECT t.*
    FROM _active_state_w_tasks AS t
    JOIN _task_causing_idle_exit AS exit USING (idle_group)
    WHERE
      t.ts >= exit.boundary_ts
  ),
  activity_islands AS (
    SELECT cpu, idle_group, min(ts) AS island_start, max(ts + dur) AS island_end
    FROM base_tasks
    GROUP BY
      cpu,
      idle_group
  ),
  swapper_gaps AS (
    SELECT
      island_end AS ts,
      lead(island_start) OVER (PARTITION BY cpu ORDER BY island_start)
      - island_end AS dur,
      cpu
    FROM activity_islands
  )
-- Combine the real tasks with the calculated swapper gaps.
--
-- `idle_group` is carried through for rows from _active_state_w_tasks since
-- they are already clipped to a single idle exit. Synthesized swapper gaps
-- can span multiple idle exits and receive NULL.
SELECT ts, dur, cpu, utid, idle_group FROM base_tasks
UNION ALL
SELECT ts, dur, cpu, 0 AS utid, NULL AS idle_group
FROM swapper_gaps
WHERE
  dur > 0;
