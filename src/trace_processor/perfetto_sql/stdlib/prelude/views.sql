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

INCLUDE PERFETTO MODULE prelude.casts;

-- Alias of the `counter` table.
CREATE PERFETTO VIEW counters(
  -- Alias of `counter.id`.
  id INT,
  -- Alias of `counter.type`.
  type STRING,
  -- Alias of `counter.ts`.
  ts LONG,
  -- Alias of `counter.track_id`.
  track_id INT,
  -- Alias of `counter.value`.
  value DOUBLE,
  -- Alias of `counter.arg_set_id`.
  arg_set_id INT,
  -- Legacy column, should no longer be used.
  name STRING,
  -- Legacy column, should no longer be used.
  unit STRING,
  -- Legacy column, should no longer be used.
  description STRING
) AS
SELECT v.*, t.name, t.unit, t.description
FROM counter v
JOIN counter_track t ON v.track_id = t.id
ORDER BY ts;

-- Contains slices from userspace which explains what threads were doing
-- during the trace.
CREATE PERFETTO VIEW slice(
  -- The id of the slice.
  id INT,
  -- The name of the "most-specific" child table containing this row.
  type STRING,
  -- The timestamp at the start of the slice (in nanoseconds).
  ts LONG,
  -- The duration of the slice (in nanoseconds).
  dur LONG,
  -- The id of the track this slice is located on.
  track_id INT,
  -- The "category" of the slice. If this slice originated with track_event,
  -- this column contains the category emitted.
  -- Otherwise, it is likely to be null (with limited exceptions).
  category STRING,
  -- The name of the slice. The name describes what was happening during the
  -- slice.
  name STRING,
  -- The depth of the slice in the current stack of slices.
  depth INT,
  -- A unique identifier obtained from the names of all slices in this stack.
  -- This is rarely useful and kept around only for legacy reasons.
  stack_id LONG,
  -- The stack_id for the parent of this slice. Rarely useful.
  parent_stack_id LONG,
  -- The id of the parent (i.e. immediate ancestor) slice for this slice.
  parent_id INT,
  -- The id of the argument set associated with this slice.
  arg_set_id INT,
  -- The thread timestamp at the start of the slice. This column will only be
  -- populated if thread timestamp collection is enabled with track_event.
  thread_ts LONG,
  -- The thread time used by this slice. This column will only be populated if
  -- thread timestamp collection is enabled with track_event.
  thread_dur LONG,
  -- The value of the CPU instruction counter at the start of the slice. This
  -- column will only be populated if thread instruction collection is enabled
  -- with track_event.
  thread_instruction_count LONG,
  -- The change in value of the CPU instruction counter between the start and
  -- end of the slice. This column will only be populated if thread instruction
  -- collection is enabled with track_event.
  thread_instruction_delta LONG,
  -- Alias of `category`.
  cat STRING,
  -- Alias of `id`.
  slice_id LONG
) AS
SELECT *, category AS cat, id AS slice_id
FROM __intrinsic_slice;

-- Contains instant events from userspace which indicates what happened at a
-- single moment in time.
CREATE PERFETTO VIEW instant(
  -- The timestamp of the instant (in nanoseconds).
  ts LONG,
  -- The id of the track this instant is located on.
  track_id INT,
  -- The name of the instant. The name describes what happened during the
  -- instant.
  name STRING,
  -- The id of the argument set associated with this instant.
  arg_set_id INT
) AS
SELECT ts, track_id, name, arg_set_id
FROM slice
WHERE dur = 0;

-- Alternative alias of table `slice`.
CREATE PERFETTO VIEW slices(
  -- Alias of `slice.id`.
  id UINT,
  -- Alias of `slice.type`.
  type STRING,
  -- Alias of `slice.ts`.
  ts LONG,
  -- Alias of `slice.dur`.
  dur LONG,
  -- Alias of `slice.track_id`.
  track_id INT,
  -- Alias of `slice.category`.
  category STRING,
  -- Alias of `slice.name`.
  name STRING,
  -- Alias of `slice.depth`.
  depth INT,
  -- Alias of `slice.stack_id`.
  stack_id LONG,
  -- Alias of `slice.parent_stack_id`.
  parent_stack_id LONG,
  -- Alias of `slice.parent_id`.
  parent_id INT,
  -- Alias of `slice.arg_set_id`.
  arg_set_id INT,
  -- Alias of `slice.thread_ts`.
  thread_ts LONG,
  -- Alias of `slice.thread_dur`.
  thread_dur LONG,
  -- Alias of `slice.thread_instruction_count`.
  thread_instruction_count LONG,
  -- Alias of `slice.thread_instruction_delta`.
  thread_instruction_delta LONG,
  -- Alias of `slice.cat`.
  cat LONG,
  -- Alias of `slice.slice_id`.
  slice_id LONG
) AS
SELECT * FROM slice;

-- Contains information of threads seen during the trace.
CREATE PERFETTO VIEW thread(
  -- The id of the thread. Prefer using `utid` instead.
  id INT,
  -- The name of the "most-specific" child table containing this row.
  type STRING,
  -- Unique thread id. This is != the OS tid. This is a monotonic number
  -- associated to each thread. The OS thread id (tid) cannot be used as primary
  -- key because tids and pids are recycled by most kernels.
  utid INT,
  -- The OS id for this thread. Note: this is *not* unique over the lifetime of
  -- the trace so cannot be used as a primary key. Use |utid| instead.
  tid INT,
  -- The name of the thread. Can be populated from many sources (e.g. ftrace,
  -- /proc scraping, track event etc).
  name STRING,
  -- The start timestamp of this thread (if known). Is null in most cases unless
  -- a thread creation event is enabled (e.g. task_newtask ftrace event on
  -- Linux/Android).
  start_ts LONG,
  -- The end timestamp of this thread (if known). Is null in most cases unless
  -- a thread destruction event is enabled (e.g. sched_process_free ftrace event
  -- on Linux/Android).
  end_ts LONG,
  -- The process hosting this thread.
  upid LONG,
  -- Boolean indicating if this thread is the main thread in the process.
  is_main_thread BOOL,
  -- Machine identifier, non-null for threads on a remote machine.
  machine_id INT
) AS
SELECT id as utid, *
FROM __intrinsic_thread;

-- Contains information of processes seen during the trace.
CREATE PERFETTO VIEW process(
  -- The id of the process. Prefer using `upid` instead.
  id INT,
  -- The name of the "most-specific" child table containing this row.
  type STRING,
  -- Unique process id. This is != the OS pid. This is a monotonic number
  -- associated to each process. The OS process id (pid) cannot be used as
  -- primary key because tids and pids are recycled by most kernels.
  upid LONG,
  -- The OS id for this process. Note: this is *not* unique over the lifetime of
  -- the trace so cannot be used as a primary key. Use |upid| instead.
  pid LONG,
  -- The name of the process. Can be populated from many sources (e.g. ftrace,
  -- /proc scraping, track event etc).
  name STRING,
  -- The start timestamp of this process (if known). Is null in most cases
  -- unless a process creation event is enabled (e.g. task_newtask ftrace event
  -- on Linux/Android).
  start_ts LONG,
  -- The end timestamp of this process (if known). Is null in most cases unless
  -- a process destruction event is enabled (e.g. sched_process_free ftrace
  -- event on Linux/Android).
  end_ts LONG,
  -- The upid of the process which caused this process to be spawned.
  parent_upid INT,
  -- The Unix user id of the process.
  uid INT,
  -- Android appid of this process.
  android_appid INT,
  -- /proc/cmdline for this process.
  cmdline STRING,
  -- Extra args for this process.
  arg_set_id INT,
  -- Machine identifier, non-null for processes on a remote machine.
  machine_id INT
) AS
SELECT id as upid, *
FROM __intrinsic_process;

-- Arbitrary key-value pairs which allow adding metadata to other, strongly
-- typed tables.
-- Note: for a given row, only one of |int_value|, |string_value|, |real_value|
-- will be non-null.
CREATE PERFETTO VIEW args(
  -- The id of the arg.
  id INT,
  -- The name of the "most-specific" child table containing this row.
  type STRING,
  -- The id for a single set of arguments.
  arg_set_id INT,
  -- The "flat key" of the arg: this is the key without any array indexes.
  flat_key STRING,
  -- The key for the arg.
  key STRING,
  -- The integer value of the arg.
  int_value LONG,
  -- The string value of the arg.
  string_value STRING,
  -- The double value of the arg.
  real_value DOUBLE,
  -- The type of the value of the arg. Will be one of 'int', 'uint', 'string',
  -- 'real', 'pointer', 'bool' or 'json'.
  value_type STRING,
  -- The human-readable formatted value of the arg.
  display_value STRING
) AS
SELECT
  *,
  -- This should be kept in sync with GlobalArgsTracker::AddArgSet.
  CASE value_type
    WHEN 'int' THEN cast_string!(int_value)
    WHEN 'uint' THEN cast_string!(int_value)
    WHEN 'string' THEN string_value
    WHEN 'real' THEN cast_string!(real_value)
    WHEN 'pointer' THEN printf('0x%x', int_value)
    WHEN 'bool' THEN (
      CASE WHEN int_value <> 0 THEN 'true'
      ELSE 'false' END)
    WHEN 'json' THEN string_value
  ELSE NULL END AS display_value
FROM __intrinsic_args;

-- Contains the Linux perf sessions in the trace.
CREATE PERFETTO VIEW perf_session(
  -- The id of the perf session. Prefer using `perf_session_id` instead.
  id INT,
  -- The name of the "most-specific" child table containing this row.
  type STRING,
  -- The id of the perf session.
  perf_session_id INT,
  -- Command line used to collect the data.
  cmdline STRING
)
AS
SELECT *, id AS perf_session_id
FROM __intrinsic_perf_session;
