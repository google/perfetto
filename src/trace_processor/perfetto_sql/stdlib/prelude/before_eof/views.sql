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

-- Callstack samples from all profiler sources (linux perf, chrome, macOS
-- instruments, the StackSample packet, ...): every profiler sample which
-- captured a callstack. Defined over the generic __intrinsic_profiler_sample
-- table, which also holds samples without callstacks (e.g. perf counter-only
-- samples); those are visible through the per-source views (e.g. perf_sample)
-- instead.
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
  -- Privilege mode the sample was taken in (e.g. "user", "kernel"). Empty if
  -- unknown.
  cpu_mode STRING,
  -- The captured callstack.
  callsite_id JOINID(stack_profile_callsite.id),
  -- The profiler session (data source instance) this sample came from, if
  -- known.
  session_id JOINID(profiler_session.id),
  -- References the set of counter values (timebase and followers) recorded
  -- at this sample point, if any.
  counter_set_id LONG
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
  session_id,
  counter_set_id
FROM __intrinsic_profiler_sample
WHERE
  callsite_id IS NOT NULL;

-- Samples from the traced_perf profiler and perf.data files. One row per perf
-- sample, including counter-only samples which have no callstack.
CREATE PERFETTO VIEW perf_sample(
  -- Unique identifier for this perf sample. Joinable with stack_sample.id.
  id ID,
  -- Timestamp of the sample.
  ts TIMESTAMP,
  -- Sampled thread.
  utid JOINID(thread.id),
  -- Core the sampled thread was running on.
  cpu LONG,
  -- Execution state (userspace/kernelspace) of the sampled thread.
  cpu_mode STRING,
  -- If set, unwound callstack of the sampled thread.
  callsite_id JOINID(stack_profile_callsite.id),
  -- If set, indicates that the unwinding for this sample encountered an error.
  -- Such samples still reference the best-effort result via the callsite_id,
  -- with a synthetic error frame at the point where unwinding stopped.
  unwind_error STRING,
  -- Distinguishes samples from different profiling streams
  -- (i.e. multiple data sources).
  perf_session_id JOINID(perf_session.id)
)
AS
SELECT
  ps.id,
  ps.ts,
  ps.utid,
  c.cpu AS cpu,
  CASE WHEN ps.cpu_mode = '' THEN 'unknown' ELSE ps.cpu_mode END AS cpu_mode,
  ps.callsite_id,
  ps.unwind_error,
  ps.session_id AS perf_session_id
FROM __intrinsic_profiler_sample AS ps
LEFT JOIN __intrinsic_cpu AS c
  ON c.id = ps.ucpu
WHERE
  ps.source = 'linux.perf';
