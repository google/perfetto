--
-- Copyright 2026 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the 'License');
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an 'AS IS' BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- Returns the counter value for a perf sample given the sample ID
-- and counter name.
CREATE PERFETTO FUNCTION linux_perf_counter_for_sample(
    -- The ID of the perf sample.
    sample_id LONG,
    -- The name of the counter (e.g., 'cpu-clock', 'instructions').
    counter_name STRING
)
-- The counter value, or NULL if not found.
RETURNS DOUBLE AS
SELECT
  __intrinsic_perf_counter_for_sample($sample_id, $counter_name);

-- Fully denormalized view joining perf samples with their counter values.
-- Note: This view has multiple rows per sample (one for each counter).
-- Use with caution for large traces as it may impact query performance.
CREATE PERFETTO VIEW linux_perf_sample_with_counters (
  -- The sample ID.
  id LONG,
  -- Timestamp of the sample.
  ts TIMESTAMP,
  -- Sampled thread ID.
  utid JOINID(thread.id),
  -- Core the sampled thread was running on.
  cpu LONG,
  -- Execution state (userspace/kernelspace).
  cpu_mode STRING,
  -- Unwound callstack of the sampled thread.
  callsite_id LONG,
  -- Stack unwinding error if any.
  unwind_error STRING,
  -- Perf session ID.
  perf_session_id LONG,
  -- Counter name (e.g., 'cpu-clock', 'instructions').
  counter_name STRING,
  -- Counter value at this sample point.
  counter_value DOUBLE,
  -- Whether this counter is the sampling timebase.
  is_timebase BOOL
) AS
SELECT
  ps.id,
  ps.ts,
  ps.utid,
  ps.cpu,
  ps.cpu_mode,
  ps.callsite_id,
  ps.unwind_error,
  ps.perf_session_id,
  pct.name AS counter_name,
  c.value AS counter_value,
  pct.is_timebase
FROM __intrinsic_perf_sample AS ps
JOIN __intrinsic_perf_counter_set AS pcs
  ON ps.counter_set_id = pcs.perf_counter_set_id
JOIN counter AS c
  ON c.id = pcs.counter_id
JOIN perf_counter_track AS pct
  ON c.track_id = pct.id;
