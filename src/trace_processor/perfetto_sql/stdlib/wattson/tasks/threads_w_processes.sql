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

INCLUDE PERFETTO MODULE intervals.overlap;

-- Establish relationships between thread/process/package
CREATE PERFETTO VIEW _tasks_summary AS
SELECT
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
  package.package_name
FROM thread
JOIN sched
  USING (utid)
LEFT JOIN process
  USING (upid)
LEFT JOIN android_process_metadata AS package
  USING (upid)
WHERE
  dur > 0;

-- Gets all IRQs and labels each slice with hard or soft IRQ
CREATE PERFETTO TABLE _all_irq_slices AS
SELECT
  id,
  ts,
  dur,
  parent_id,
  extract_arg(arg_set_id, 'vec') IS NULL AS is_hard_irq
FROM slice
WHERE
  category = 'irq';

-- Flatten hard IRQs, since they can preempt each other. Only hard IRQs can use
-- the built-in ancestor_slice() and interval functions
CREATE PERFETTO VIEW _hard_irq_flattened_slices AS
WITH
  root_slices AS (
    SELECT
      id,
      ts,
      dur
    FROM _all_irq_slices
    WHERE
      parent_id IS NULL AND is_hard_irq
  ),
  child_slices AS (
    SELECT
      anc.id AS root_id,
      irq.id,
      irq.parent_id,
      irq.ts,
      irq.dur
    FROM _all_irq_slices AS irq, ancestor_slice(irq.id) AS anc
    WHERE
      NOT irq.parent_id IS NULL AND is_hard_irq
  )
SELECT
  intervals.ts,
  intervals.dur,
  slices.name AS hard_irq_name,
  slices.stack_id AS hard_irq_stack_id,
  cpu_track.cpu
FROM _intervals_flatten!(_intervals_merge_root_and_children!(root_slices, child_slices)) AS intervals
JOIN slices
  USING (id)
JOIN cpu_track
  ON cpu_track.id = slices.track_id;

-- Softirqs run with other softirqs disabled, so will not be preempted by each
-- other, and thus do not need to be flattened like hard IRQs do.
CREATE PERFETTO VIEW _soft_irq_slices AS
SELECT
  _all_irq_slices.ts,
  _all_irq_slices.dur,
  slices.name AS soft_irq_name,
  slices.stack_id AS soft_irq_stack_id,
  cpu_track.cpu
FROM _all_irq_slices
JOIN slices
  USING (id)
JOIN cpu_track
  ON cpu_track.id = slices.track_id
WHERE
  NOT is_hard_irq;

CREATE VIRTUAL TABLE _all_irqs_combined_slices USING SPAN_OUTER_JOIN (
  _soft_irq_slices PARTITIONED cpu,
  _hard_irq_flattened_slices PARTITIONED cpu
);

-- Replace soft IRQs with hard IRQs if hard IRQs are present. Hard IRQs could
-- preempt soft IRQs, but not the other way around.
CREATE PERFETTO VIEW _all_irqs_flattened_slices AS
WITH
  base_name AS (
    SELECT
      ts,
      dur,
      cpu,
      coalesce(hard_irq_name, soft_irq_name) AS irq_name,
      coalesce(hard_irq_stack_id, soft_irq_stack_id) AS stack_id
    FROM _all_irqs_combined_slices
  )
SELECT
  ts,
  dur,
  cpu,
  irq_name,
  -- Default max PID on Linux is 32768, so add this to reduce possibility of
  -- this synthetic irq_id clashing with PIDs
  stack_id + 32768 AS irq_id
FROM base_name;

-- SPAN_OUTER_JOIN needed because IRQ table do not have contiguous slices,
-- whereas tasks table will be contiguous
CREATE VIRTUAL TABLE _irq_w_tasks_info USING SPAN_OUTER_JOIN (
  _tasks_summary PARTITIONED cpu,
  _all_irqs_flattened_slices PARTITIONED cpu
);

-- Replace nominal tasks with IRQ if the IRQ slice is present. IRQs could
-- preempt tasks, but not the other way around.
CREATE PERFETTO TABLE _sched_w_thread_process_package_summary AS
SELECT
  ts,
  dur,
  cpu,
  coalesce(irq_id, utid) AS utid,
  coalesce(irq_id, upid) AS upid,
  coalesce(irq_id, tid) AS tid,
  coalesce(irq_id, pid) AS pid,
  coalesce(irq_id, uid) AS uid,
  coalesce(irq_name, thread_name) AS thread_name,
  coalesce(irq_name, process_name) AS process_name,
  coalesce(irq_name, package_name) AS package_name
FROM _irq_w_tasks_info;
