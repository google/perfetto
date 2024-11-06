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

INCLUDE PERFETTO MODULE prelude.after_eof.views;

-- Lists all metrics built-into trace processor.
CREATE PERFETTO VIEW trace_metrics(
  -- The name of the metric.
  name STRING
) AS
SELECT name FROM _trace_metrics;

-- Definition of `trace_bounds` table. The values are being filled by Trace
-- Processor when parsing the trace.
-- It is recommended to depend on the `trace_start()` and `trace_end()`
-- functions rather than directly on `trace_bounds`.
CREATE PERFETTO VIEW trace_bounds(
  -- First ts in the trace.
  start_ts INT,
  -- End of the trace.
  end_ts INT
) AS
SELECT start_ts, end_ts FROM _trace_bounds;

-- Tracks are a fundamental concept in trace processor and represent a
-- "timeline" for events of the same type and with the same context. See
-- https://perfetto.dev/docs/analysis/trace-processor#tracks for a more
-- detailed explanation, with examples.
CREATE PERFETTO VIEW track (
  -- Unique identifier for this track. Identical to |track_id|, prefer using
  -- |track_id| instead.
  id UINT,
  -- The name of the "most-specific" child table containing this row.
  type STRING,
  -- Name of the track; can be null for some types of tracks (e.g. thread
  -- tracks).
  name STRING,
  -- The classification of a track indicates the "type of data" the track
  -- contains.
  --
  -- Every track is uniquely identified by the the combination of the
  -- classification and a set of dimensions: classifications allow identifying
  -- a set of tracks with the same type of data within the whole universe of
  -- tracks while dimensions allow distinguishing between different tracks in
  -- that set.
  classification STRING,
  -- The dimensions of the track which uniquely identify the track within a
  -- given classification.
  --
  -- Join with the `args` table or use the `EXTRACT_ARG` helper function to
  -- expand the args.
  dimension_arg_set_id UINT,
  -- The track which is the "parent" of this track. Only non-null for tracks
  -- created using Perfetto's track_event API.
  parent_id UINT,
  -- Generic key-value pairs containing extra information about the track.
  --
  -- Join with the `args` table or use the `EXTRACT_ARG` helper function to
  -- expand the args.
  source_arg_set_id UINT,
  -- Machine identifier, non-null for tracks on a remote machine.
  machine_id UINT
) AS
SELECT
  id,
  type,
  name,
  classification,
  dimension_arg_set_id,
  parent_id,
  source_arg_set_id,
  machine_id
FROM __intrinsic_track;

-- Contains information about the CPUs on the device this trace was taken on.
CREATE PERFETTO VIEW cpu (
  -- Unique identifier for this CPU. Identical to |ucpu|, prefer using |ucpu|
  -- instead.
  id UINT,
  -- Unique identifier for this CPU. Isn't equal to |cpu| for remote machines
  -- and is equal to |cpu| for the host machine.
  ucpu UINT,
  -- The 0-based CPU core identifier.
  cpu UINT,
  -- The name of the "most-specific" child table containing this row.
  type STRING,
  -- The cluster id is shared by CPUs in the same cluster.
  cluster_id UINT,
  -- A string describing this core.
  processor STRING,
  -- Machine identifier, non-null for CPUs on a remote machine.
  machine_id UINT,
  -- Capacity of a CPU of a device, a metric which indicates the
  -- relative performance of a CPU on a device
  -- For details see:
  -- https://www.kernel.org/doc/Documentation/devicetree/bindings/arm/cpu-capacity.txt
  capacity UINT,
  -- Extra key/value pairs associated with this cpu.
  arg_set_id UINT
) AS
SELECT
  id,
  id AS ucpu,
  cpu,
  type AS type,
  cluster_id,
  processor,
  machine_id,
  capacity,
  arg_set_id
FROM
  __intrinsic_cpu
WHERE
  cpu IS NOT NULL;

-- Contains the frequency values that the CPUs on the device are capable of
-- running at.
CREATE PERFETTO VIEW cpu_available_frequencies (
  -- Unique identifier for this cpu frequency.
  id UINT,
  -- The CPU for this frequency, meaningful only in single machine traces.
  -- For multi-machine, join with the `cpu` table on `ucpu` to get the CPU
  -- identifier of each machine.
  cpu UINT,
  -- CPU frequency in KHz.
  freq UINT,
  -- The CPU that the slice executed on (meaningful only in single machine
  -- traces). For multi-machine, join with the `cpu` table on `ucpu` to get the
  -- CPU identifier of each machine.
  ucpu UINT
) AS
SELECT
  id,
  ucpu AS cpu,
  freq,
  ucpu
FROM
  __intrinsic_cpu_freq;

-- This table holds slices with kernel thread scheduling information. These
-- slices are collected when the Linux "ftrace" data source is used with the
-- "sched/switch" and "sched/wakeup*" events enabled.
--
-- The rows in this table will always have a matching row in the |thread_state|
-- table with |thread_state.state| = 'Running'
CREATE PERFETTO VIEW sched_slice (
  --  Unique identifier for this scheduling slice.
  id UINT,
  -- The name of the "most-specific" child table containing this row.
  type STRING,
  -- The timestamp at the start of the slice (in nanoseconds).
  ts LONG,
  -- The duration of the slice (in nanoseconds).
  dur LONG,
  -- The CPU that the slice executed on (meaningful only in single machine
  -- traces). For multi-machine, join with the `cpu` table on `ucpu` to get the
  -- CPU identifier of each machine.
  cpu UINT,
  -- The thread's unique id in the trace.
  utid UINT,
  -- A string representing the scheduling state of the kernel
  -- thread at the end of the slice.  The individual characters in
  -- the string mean the following: R (runnable), S (awaiting a
  -- wakeup), D (in an uninterruptible sleep), T (suspended),
  -- t (being traced), X (exiting), P (parked), W (waking),
  -- I (idle), N (not contributing to the load average),
  -- K (wakeable on fatal signals) and Z (zombie, awaiting
  -- cleanup).
  end_state STRING,
  -- The kernel priority that the thread ran at.
  priority INT,
  -- The unique CPU identifier that the slice executed on.
  ucpu UINT
) AS
SELECT
  id,
  type,
  ts,
  dur,
  ucpu AS cpu,
  utid,
  end_state,
  priority,
  ucpu
FROM
  __intrinsic_sched_slice;

-- Shorter alias for table `sched_slice`.
CREATE PERFETTO VIEW sched(
  -- Alias for `sched_slice.id`.
  id UINT,
  -- Alias for `sched_slice.type`.
  type STRING,
  -- Alias for `sched_slice.ts`.
  ts LONG,
  -- Alias for `sched_slice.dur`.
  dur LONG,
  -- Alias for `sched_slice.cpu`.
  cpu UINT,
  -- Alias for `sched_slice.utid`.
  utid UINT,
  -- Alias for `sched_slice.end_state`.
  end_state STRING,
  -- Alias for `sched_slice.priority`.
  priority INT,
  -- Alias for `sched_slice.ucpu`.
  ucpu UINT,
  -- Legacy column, should no longer be used.
  ts_end UINT
) AS
SELECT *, ts + dur as ts_end
FROM sched_slice;

-- This table contains the scheduling state of every thread on the system during
-- the trace.
--
-- The rows in this table which have |state| = 'Running', will have a
-- corresponding row in the |sched_slice| table.
CREATE PERFETTO VIEW thread_state (
  -- Unique identifier for this thread state.
  id UINT,
  -- The name of the "most-specific" child table containing this row.
  type STRING,
  -- The timestamp at the start of the slice (in nanoseconds).
  ts LONG,
  -- The duration of the slice (in nanoseconds).
  dur LONG,
  -- The CPU that the thread executed on (meaningful only in single machine
  -- traces). For multi-machine, join with the `cpu` table on `ucpu` to get the
  -- CPU identifier of each machine.
  cpu UINT,
  -- The thread's unique id in the trace.
  utid UINT,
  -- The scheduling state of the thread. Can be "Running" or any of the states
  -- described in |sched_slice.end_state|.
  state STRING,
  -- Indicates whether this thread was blocked on IO.
  io_wait UINT,
  -- The function in the kernel this thread was blocked on.
  blocked_function STRING,
  -- The unique thread id of the thread which caused a wakeup of this thread.
  waker_utid UINT,
  -- The unique thread state id which caused a wakeup of this thread.
  waker_id UINT,
  -- Whether the wakeup was from interrupt context or process context.
  irq_context UINT,
  -- The unique CPU identifier that the thread executed on.
  ucpu UINT
) AS
SELECT
  id,
  type,
  ts,
  dur,
  ucpu AS cpu,
  utid,
  state,
  io_wait,
  blocked_function,
  waker_utid,
  waker_id,
  irq_context,
  ucpu
FROM
  __intrinsic_thread_state;

-- Contains 'raw' events from the trace for some types of events. This table
-- only exists for debugging purposes and should not be relied on in production
-- usecases (i.e. metrics, standard library etc.)
CREATE PERFETTO VIEW raw (
  -- Unique identifier for this raw event.
  id UINT,
  -- The name of the "most-specific" child table containing this row.
  type STRING,
  -- The timestamp of this event.
  ts LONG,
  -- The name of the event. For ftrace events, this will be the ftrace event
  -- name.
  name STRING,
  -- The CPU this event was emitted on (meaningful only in single machine
  -- traces). For multi-machine, join with the `cpu` table on `ucpu` to get the
  -- CPU identifier of each machine.
  cpu UINT,
  -- The thread this event was emitted on.
  utid UINT,
  -- The set of key/value pairs associated with this event.
  arg_set_id UINT,
  -- Ftrace event flags for this event. Currently only emitted for sched_waking
  -- events.
  common_flags UINT,
  -- The unique CPU identifier that this event was emitted on.
  ucpu UINT
) AS
SELECT
  id,
  type,
  ts,
  name,
  ucpu AS cpu,
  utid,
  arg_set_id,
  common_flags,
  ucpu
FROM
  __intrinsic_raw;

-- Contains all the ftrace events in the trace. This table exists only for
-- debugging purposes and should not be relied on in production usecases (i.e.
-- metrics, standard library etc). Note also that this table might be empty if
-- raw ftrace parsing has been disabled.
CREATE PERFETTO VIEW ftrace_event (
  -- Unique identifier for this ftrace event.
  id UINT,
  -- The name of the "most-specific" child table containing this row.
  type STRING,
  -- The timestamp of this event.
  ts LONG,
  -- The ftrace event name.
  name STRING,
  -- The CPU this event was emitted on (meaningful only in single machine
  -- traces). For multi-machine, join with the `cpu` table on `ucpu` to get the
  -- CPU identifier of each machine.
  cpu UINT,
  -- The thread this event was emitted on.
  utid UINT,
  -- The set of key/value pairs associated with this event.
  arg_set_id UINT,
  -- Ftrace event flags for this event. Currently only emitted for
  -- sched_waking events.
  common_flags UINT,
  -- The unique CPU identifier that this event was emitted on.
  ucpu UINT
) AS
SELECT
  id,
  type,
  ts,
  name,
  ucpu AS cpu,
  utid,
  arg_set_id,
  common_flags,
  ucpu
FROM
  __intrinsic_ftrace_event;

-- The sched_slice table with the upid column.
CREATE PERFETTO VIEW experimental_sched_upid (
  --  Unique identifier for this scheduling slice.
  id UINT,
  -- The name of the "most-specific" child table containing this row.
  type STRING,
  -- The timestamp at the start of the slice (in nanoseconds).
  ts LONG,
  -- The duration of the slice (in nanoseconds).
  dur LONG,
  -- The CPU that the slice executed on (meaningful only in single machine
  -- traces). For multi-machine, join with the `cpu` table on `ucpu` to get the
  -- CPU identifier of each machine.
  cpu UINT,
  -- The thread's unique id in the trace.
  utid UINT,
  -- A string representing the scheduling state of the kernel thread at the end
  -- of the slice. The individual characters in the string mean the following: R
  -- (runnable), S (awaiting a wakeup), D (in an uninterruptible sleep), T
  -- (suspended), t (being traced), X (exiting), P (parked), W (waking), I
  -- (idle), N (not contributing to the load average), K (wakeable on fatal
  -- signals) and Z (zombie, awaiting cleanup).
  end_state STRING,
  -- The kernel priority that the thread ran at.
  priority INT,
  -- The unique CPU identifier that the slice executed on.
  ucpu UINT,
  -- The process's unique id in the trace.
  upid UINT
) AS
SELECT
  id,
  type,
  ts,
  dur,
  ucpu AS cpu,
  utid,
  end_state,
  priority,
  ucpu,
  upid
FROM
  __intrinsic_sched_upid;

-- Tracks which are associated to a single CPU.
CREATE PERFETTO VIEW cpu_track (
  -- Unique identifier for this cpu track.
  id UINT,
  -- The name of the "most-specific" child table containing this row.
  type STRING,
  -- Name of the track.
  name STRING,
  -- The track which is the "parent" of this track. Only non-null for tracks
  -- created using Perfetto's track_event API.
  parent_id UINT,
  -- Args for this track which store information about "source" of this track in
  -- the trace. For example: whether this track orginated from atrace, Chrome
  -- tracepoints etc.
  source_arg_set_id UINT,
  -- Machine identifier, non-null for tracks on a remote machine.
  machine_id UINT,
  -- The CPU that the track is associated with (meaningful only in single
  -- machine traces). For multi-machine, join with the `cpu` table on `ucpu` to
  -- get the CPU identifier of each machine.
  cpu UINT,
  -- The unique CPU identifier that this track is associated with.
  ucpu UINT
) AS
SELECT
  id,
  type,
  name,
  parent_id,
  source_arg_set_id,
  machine_id,
  ucpu AS cpu,
  ucpu
FROM
  __intrinsic_cpu_track;

-- Tracks containing counter-like events associated to a CPU.
CREATE PERFETTO VIEW cpu_counter_track (
  -- Unique identifier for this cpu counter track.
  id UINT,
  -- The name of the "most-specific" child table containing this row.
  type STRING,
  -- Name of the track.
  name STRING,
  -- The track which is the "parent" of this track. Only non-null for tracks
  -- created using Perfetto's track_event API.
  parent_id UINT,
  -- Args for this track which store information about "source" of this track in
  -- the trace. For example: whether this track orginated from atrace, Chrome
  -- tracepoints etc.
  source_arg_set_id UINT,
  -- Machine identifier, non-null for tracks on a remote machine.
  machine_id UINT,
  -- The units of the counter. This column is rarely filled.
  unit STRING,
  -- The description for this track. For debugging purposes only.
  description STRING,
  -- The CPU that the track is associated with (meaningful only in single
  -- machine traces). For multi-machine, join with the `cpu` table on `ucpu` to
  -- get the CPU identifier of each machine.
  cpu UINT,
  -- The unique CPU identifier that this track is associated with.
  ucpu UINT
) AS
SELECT
  id,
  type,
  name,
  parent_id,
  source_arg_set_id,
  machine_id,
  unit,
  description,
  ucpu AS cpu,
  ucpu
FROM
  __intrinsic_cpu_counter_track;
