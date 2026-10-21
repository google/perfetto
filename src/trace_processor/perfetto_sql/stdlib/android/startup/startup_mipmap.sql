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

INCLUDE PERFETTO MODULE intervals.mipmap;

INCLUDE PERFETTO MODULE android.startup.startups;

INCLUDE PERFETTO MODULE android.process_metadata;

INCLUDE PERFETTO MODULE slices.flat_slices;

INCLUDE PERFETTO MODULE sched.states;

-- Create a table with unique startup events, including thread and process information.
CREATE PERFETTO TABLE _mipmap_startup AS
SELECT android_startups.startup_id AS id, android_startups.*, upid, utid
FROM android_startups
JOIN android_startup_processes USING (startup_id)
JOIN thread USING (upid)
JOIN android_process_metadata USING (upid)
WHERE
  dur > 0
  AND is_main_thread
ORDER BY
  ts;

-- Flattened slices on the threads of the startups. Stretches with no slice
-- are filled with a NULL slice so that thread states outside of any slice are
-- kept when intersecting below.
CREATE PERFETTO TABLE _mipmap_flat_slice AS
FROM (
  SELECT f.slice_id, f.ts, f.dur, f.utid, f.depth, f.name
  FROM _slice_flattened AS f
  JOIN track
    ON f.track_id = track.id
  WHERE
    track.type = 'thread_execution'
    AND f.utid IN (SELECT utid FROM _mipmap_startup)
)
|> INTERVAL FILL GAPS WITH (
  SELECT DISTINCT utid, trace_start() AS ts, trace_dur() AS dur
  FROM _mipmap_startup
) AS bg PER utid;

-- Intersect the thread states, the flattened slices and the startups.
CREATE PERFETTO TABLE _mipmap_startup_slice_raw AS
INTERVAL INTERSECTION OF (
  (
    -- Thread states still open at the end of the trace have dur = -1.
    SELECT
      ts,
      iif(dur = -1, trace_end() - ts, dur) AS dur,
      utid,
      state,
      cpu,
      io_wait,
      blocked_function,
      irq_context
    FROM thread_state
    WHERE
      utid IN (SELECT utid FROM _mipmap_startup)
  ) AS t,
  _mipmap_flat_slice AS f,
  (
    SELECT
      ts,
      dur,
      utid,
      startup_id,
      upid,
      startup_type,
      package,
      dur AS startup_dur
    FROM _mipmap_startup
  ) AS s
) PER utid
|> SELECT
  ts,
  dur,
  utid,
  f.slice_id AS slice_id,
  f.depth AS depth,
  f.name AS name,
  t.state AS state,
  t.cpu AS cpu,
  t.io_wait AS io_wait,
  t.blocked_function AS blocked_function,
  t.irq_context AS irq_context,
  s.startup_id AS startup_id,
  s.upid AS upid,
  s.startup_type AS startup_type,
  s.package AS package,
  s.startup_dur AS startup_dur;

-- The slices with thread states that occurred during each startup.
CREATE PERFETTO VIEW _mipmap_startup_slice AS
SELECT
  ts,
  dur,
  utid,
  slice_id,
  depth,
  name,
  coalesce(name, sched_state_to_human_readable_string(state)) AS synth_name,
  cpu,
  sched_state_to_human_readable_string(state) AS state,
  io_wait,
  blocked_function,
  irq_context,
  startup_id,
  upid,
  startup_type,
  package,
  startup_dur
FROM _mipmap_startup_slice_raw;

-- ------------------------------------------------------------------
-- MIPMAP Generation Call
-- ------------------------------------------------------------------

--
-- Creates 1ms buckets for startup intervals.
--
-- This table uses the `_mipmap_buckets_table` macro to generate a series of
-- 1-ms buckets for each startup. These buckets will be used to
-- aggregate and summarize the startup activity.
CREATE PERFETTO TABLE _mipmap_startup_buckets_1ms AS
SELECT *
FROM _mipmap_buckets_table!(
  -- Source table for time range
  (SELECT ts, dur, startup_id FROM _mipmap_startup_slice),
  -- Partitioning column
  startup_id,
  -- Bucket duration in nanoseconds
  1e6  -- 1ms buckets
)
ORDER BY
  id;

--
-- Prepares startup slices for mipmapping.
--
-- This table assigns a unique ID and a `group_hash` to each startup slice. The
-- `group_hash` is created from various properties of the slice, such as its
-- name, depth, and thread state. Slices with the same `group_hash` are
-- considered similar and can be merged during the mipmapping process.
CREATE PERFETTO TABLE _mipmap_startup_slices_with_ids AS
SELECT
  row_number() OVER (ORDER BY ts) AS id,
  hash(
    coalesce(name, ''),
    coalesce(state, ''),
    coalesce(depth, ''),
    coalesce(io_wait, ''),
    coalesce(blocked_function, '')
  ) AS group_hash,
  *
FROM _mipmap_startup_slice
ORDER BY
  id;

-- Creates a 1ms resolution mipmap of Android startup slices.
--
-- This table uses the `_mipmap_merged` macro to generate a
-- mipmap of the startup slices. The mipmap provides a summarized view of the
-- startup, with a resolution of 1 ms. The table contains merged slices
-- representing the dominant event in each time bucket.
CREATE PERFETTO TABLE _android_startup_mipmap_1ms(
  -- timestamp of the merged slice
  ts TIMESTAMP,
  -- duration of the merged slice
  dur DURATION,
  -- unique startup id
  startup_id JOINID(android_startups.startup_id),
  -- upid of the startup
  upid LONG,
  -- package name
  package STRING,
  -- startup type
  startup_type STRING,
  -- original startup duration
  startup_dur DURATION,
  -- slice name of the dominant event
  name STRING,
  -- thread state of the dominant event
  state STRING,
  -- slice depth of the dominant event
  depth LONG,
  -- whether the thread was in io_wait
  io_wait LONG,
  -- blocked function
  blocked_function STRING
)
AS
SELECT
  mm.ts,
  mm.dur,
  s.startup_id,
  s.upid,
  s.package,
  s.startup_type,
  s.startup_dur,
  -- properties from the representative slice, must be present in the group_hash
  s.name,
  s.state,
  s.depth,
  s.io_wait,
  s.blocked_function
FROM _mipmap_merged!(
  _mipmap_startup_slices_with_ids,
  _mipmap_startup_buckets_1ms,
  startup_id,
  1e6  -- 1ms buckets
) AS mm
JOIN _mipmap_startup_slices_with_ids AS s USING (id);
