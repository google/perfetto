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

INCLUDE PERFETTO MODULE linux.cpu.utilization.slice;

INCLUDE PERFETTO MODULE slices.with_context;

-- Time each thread slice spent running on CPU.
-- Requires scheduling data to be available in the trace.
CREATE PERFETTO PIPELINE thread_slice_cpu_time(
  -- Slice.
  id JOINID(slice.id),
  -- Name of the slice.
  name STRING,
  -- Id of the thread the slice is running on.
  utid JOINID(thread.id),
  -- Name of the thread.
  thread_name STRING,
  -- Id of the process the slice is running on.
  upid JOINID(process.id),
  -- Name of the process.
  process_name STRING,
  -- Duration of the time the slice was running.
  cpu_time LONG
)
MATERIALIZED AS
SUBPIPELINE thread_slices AS (
  FROM thread_slice
  |> WHERE utid > 0 AND dur > 0
)
SUBPIPELINE running AS (
  FROM sched
  |> WHERE dur > 0
)
INTERVAL INTERSECTION OF (thread_slices AS ts, running AS r) PER utid
|> AGGREGATE
  MIN(ts.name) AS name,
  MIN(ts.thread_name) AS thread_name,
  MIN(ts.upid) AS upid,
  MIN(ts.process_name) AS process_name,
  SUM(dur) AS cpu_time
  GROUP BY ts.id, ts.utid
|> SELECT
  ts.id AS id,
  name,
  ts.utid AS utid,
  thread_name,
  upid,
  process_name,
  cpu_time;

-- CPU cycles per each slice.
CREATE PERFETTO PIPELINE thread_slice_cpu_cycles(
  -- Id of a slice.
  id JOINID(slice.id),
  -- Name of the slice.
  name STRING,
  -- Id of the thread the slice is running on.
  utid JOINID(thread.id),
  -- Name of the thread.
  thread_name STRING,
  -- Id of the process the slice is running on.
  upid JOINID(process.id),
  -- Name of the process.
  process_name STRING,
  -- Sum of CPU millicycles. Null if frequency couldn't be fetched for any
  -- period during the runtime of the slice.
  millicycles LONG,
  -- Sum of CPU megacycles. Null if frequency couldn't be fetched for any
  -- period during the runtime of the slice.
  megacycles LONG
)
AS
FROM cpu_cycles_per_thread_slice
|> SELECT id, name, utid, thread_name, upid, process_name, millicycles, megacycles;
