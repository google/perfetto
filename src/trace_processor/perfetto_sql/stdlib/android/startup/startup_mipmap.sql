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

INCLUDE PERFETTO MODULE android.startup.mipmap;

INCLUDE PERFETTO MODULE android.startup.startups;

INCLUDE PERFETTO MODULE android.process_metadata;

INCLUDE PERFETTO MODULE intervals.intersect;

INCLUDE PERFETTO MODULE slices.flat_slices;

INCLUDE PERFETTO MODULE sched.states;

CREATE PERFETTO TABLE _unique_startup AS
WITH
  x AS (
    SELECT
      android_startups.startup_id AS id,
      android_startups.*,
      upid,
      utid
    FROM android_startups
    JOIN android_startup_processes
      USING (startup_id)
    JOIN thread
      USING (upid)
    JOIN android_process_metadata
      USING (upid)
    WHERE
      dur > 0 AND is_main_thread
    ORDER BY
      ts
  )
SELECT
  *,
  id AS unique_startup_id
FROM x;

CREATE PERFETTO TABLE _flat_slice AS
SELECT
  _slice_flattened.*
FROM _slice_flattened
JOIN _unique_startup
  USING (utid);

CREATE VIRTUAL TABLE _flat_slice_thread_states_and_slices_sp USING SPAN_LEFT_JOIN (
    thread_state PARTITIONED utid,
    _flat_slice PARTITIONED utid);

CREATE PERFETTO TABLE _flat_slice_thread_states_and_slices AS
SELECT
  row_number() OVER () AS id,
  ts,
  dur,
  utid,
  slice_id,
  depth,
  name,
  sched_state_to_human_readable_string(state) AS state,
  cpu,
  io_wait,
  blocked_function,
  coalesce(name, sched_state_to_human_readable_string(state)) AS synth_name,
  irq_context
FROM _flat_slice_thread_states_and_slices_sp;

CREATE PERFETTO TABLE _startup_slice AS
SELECT
  ii.ts,
  ii.dur,
  slice.utid,
  slice.id AS slice_id,
  slice.depth,
  slice.name,
  slice.synth_name,
  slice.cpu,
  slice.state,
  slice.io_wait,
  slice.blocked_function,
  slice.irq_context,
  us.startup_type,
  us.package,
  us.dur AS startup_dur
FROM _interval_intersect
    !(
      (
        (SELECT * FROM _flat_slice_thread_states_and_slices WHERE dur > -1), (_unique_startup)),
      (utid)) AS ii
JOIN _flat_slice_thread_states_and_slices AS slice
  ON slice.id = ii.id_0
JOIN _unique_startup AS us
  ON us.id = ii.id_1;

-- MIPMAP call

CREATE PERFETTO TABLE _mm_startup_buckets_1ms AS
SELECT
  *
FROM _mm_buckets_table!(
  (SELECT ts, dur FROM _startup_slice),
  1e6 -- 1ms buckets
)
ORDER BY
  id;

CREATE PERFETTO TABLE _startup_slices_with_ids AS
SELECT
  row_number() OVER (ORDER BY ts) AS id,
  coalesce(package, '') || '|' || coalesce(startup_type, '') || '|' || coalesce(name, '') || '|' || coalesce(state, '') || '|' || coalesce(depth, '') || '|' || coalesce(io_wait, '') || '|' || coalesce(blocked_function, '') AS group_hash,
  *
FROM _startup_slice
ORDER BY
  id;

--
-- Startup MIPMAP 1ms: Creates 1ms resolution mipmap of android startup slices.
--
CREATE PERFETTO TABLE android_startup_mipmap_1ms (
  -- timestamp of the bucket
  ts TIMESTAMP,
  -- duration of the bucket
  dur LONG,
  -- slice name
  name STRING,
  -- thread state
  state STRING,
  -- slice depth
  depth LONG,
  -- whether the thread was in io_wait
  io_wait LONG,
  -- blocked function
  blocked_function STRING,
  -- package name
  package STRING,
  -- startup type
  startup_type STRING,
  -- startup duration
  startup_dur LONG
) AS
SELECT
  mm.ts,
  mm.dur,
  s.name,
  s.state,
  s.depth,
  s.io_wait,
  s.blocked_function,
  s.package,
  s.startup_type,
  s.startup_dur
FROM _mm_merged!(
  _startup_slices_with_ids,
  _mm_startup_buckets_1ms,
  1e6  -- 1ms buckets
) AS mm
JOIN _startup_slices_with_ids AS s
  USING (id);
