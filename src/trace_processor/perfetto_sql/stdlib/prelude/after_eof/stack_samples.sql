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

INCLUDE PERFETTO MODULE prelude.after_eof.counters;

-- A sampling session: one row per data source instance of a stack profiler
-- (a linux perf session, a StackSample packet stream, ...).
CREATE PERFETTO TABLE stack_sample_session(
  -- Unique identifier for this session.
  id ID,
  -- The profiler that produced this session's samples (e.g. "linux.perf").
  source STRING,
  -- Unit of the quantity the profiler sampled on: the session's primary
  -- (timebase) counter (e.g. "ns", "cycles", "instructions", "count").
  -- NULL if unknown.
  timebase_unit STRING,
  -- Command line used to collect the data, if known.
  cmdline STRING
)
AS
SELECT s.id, s.source, s.timebase_unit, s.cmdline
FROM __intrinsic_profiler_session AS s
WHERE
  s.id IN (
    SELECT session_id
    FROM __intrinsic_profiler_sample
    WHERE
      callsite_id IS NOT NULL
      AND session_id IS NOT NULL
  );

-- Callstack samples from all profiler sources (linux perf, chrome, macOS
-- instruments, the StackSample packet, ...): every profiler sample which
-- captured a callstack. The underlying storage also holds samples without
-- callstacks (e.g. perf counter-only samples); those remain visible through
-- per-source views such as perf_sample.
CREATE PERFETTO VIEW stack_sample(
  -- Unique identifier for this sample.
  id ID,
  -- Timestamp of the sample.
  ts TIMESTAMP,
  -- The profiler that produced the sample (e.g. "linux.perf", "chrome",
  -- "instruments").
  source STRING,
  -- The sampled thread, if known.
  utid JOINID(thread.id),
  -- The sampled process, if known.
  upid JOINID(process.id),
  -- Name of the stackful async context (goroutine, fiber, ...), if the
  -- sample is attributed to one.
  async_name STRING,
  -- Kind of the async context, e.g. "goroutine".
  async_kind STRING,
  -- Unique core the sample was taken on, if known.
  ucpu JOINID(cpu.id),
  -- Privilege mode the sample was taken in (e.g. "user", "kernel"). NULL if
  -- unknown.
  cpu_mode STRING,
  -- The captured callstack.
  callsite_id JOINID(stack_profile_callsite.id),
  -- The sampling session this sample came from, if known.
  session_id JOINID(stack_sample_session.id)
)
AS
SELECT
  id,
  ts,
  source,
  utid,
  upid,
  async_name,
  async_kind,
  ucpu,
  cpu_mode,
  callsite_id,
  session_id
FROM __intrinsic_profiler_sample
WHERE
  callsite_id IS NOT NULL;

-- Counter tracks whose values are associated with stack samples. A track is a
-- counter instance within one sampling session and can optionally be scoped to
-- a CPU.
CREATE PERFETTO TABLE stack_sample_counter_track(
  -- Unique identifier for this counter track.
  id ID(track.id),
  -- Name of the counter.
  name STRING,
  -- Type of the underlying track.
  type STRING,
  -- Parent track, if any.
  parent_id JOINID(track.id),
  -- Args describing the source of the track.
  source_arg_set_id ARGSETID,
  -- Machine identifier.
  machine_id JOINID(machine.id),
  -- Unit of the counter values.
  unit STRING,
  -- Human-readable description, if present.
  description STRING,
  -- Sampling session this counter belongs to.
  session_id JOINID(stack_sample_session.id),
  -- CPU this counter instance is scoped to, if any.
  cpu LONG,
  -- Whether this counter is the sampling timebase for the session.
  is_timebase BOOL
)
AS
SELECT
  ct.id,
  ct.name,
  ct.type,
  ct.parent_id,
  ct.source_arg_set_id,
  ct.machine_id,
  ct.unit,
  ct.description,
  coalesce(
    extract_arg(ct.dimension_arg_set_id, 'session_id'),
    extract_arg(ct.dimension_arg_set_id, 'perf_session_id')
  ) AS session_id,
  extract_arg(ct.dimension_arg_set_id, 'cpu') AS cpu,
  extract_arg(ct.source_arg_set_id, 'is_timebase') AS is_timebase
FROM counter_track AS ct
WHERE
  ct.id IN (
    SELECT c.track_id
    FROM __intrinsic_profiler_sample AS ps
    JOIN __intrinsic_profiler_counter_set AS pcs
      ON pcs.counter_set_id = ps.counter_set_id
    JOIN counter AS c
      ON c.id = pcs.counter_id
    WHERE
      ps.callsite_id IS NOT NULL
  );

-- Counter values recorded at a stack sample point. A sample can have one
-- primary timebase counter and any number of follower counters.
CREATE PERFETTO VIEW stack_sample_counter(
  -- Unique identifier for this counter value.
  id ID(counter.id),
  -- Stack sample this value was recorded with.
  stack_sample_id JOINID(stack_sample.id),
  -- Counter instance this value belongs to.
  track_id JOINID(stack_sample_counter_track.id),
  -- Counter value recorded at the sample point.
  value DOUBLE
)
AS
SELECT c.id, ps.id AS stack_sample_id, c.track_id, c.value
FROM __intrinsic_profiler_sample AS ps
JOIN __intrinsic_profiler_counter_set AS pcs
  ON pcs.counter_set_id = ps.counter_set_id
JOIN __intrinsic_counter AS c
  ON c.id = pcs.counter_id
WHERE
  ps.callsite_id IS NOT NULL;
