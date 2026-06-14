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
CREATE PERFETTO PIPELINE _mipmap_startup MATERIALIZED AS
FROM android_startups
|> JOIN android_startup_processes USING (startup_id)
|> JOIN thread USING (upid)
|> JOIN android_process_metadata USING (upid)
|> WHERE dur > 0 AND is_main_thread
|> SELECT android_startups.startup_id AS id, android_startups.*, upid, utid
|> ORDER BY ts;

-- Flatten slices that occur within the startups.
CREATE PERFETTO PIPELINE _mipmap_flat_slice MATERIALIZED AS
FROM _slice_flattened
|> JOIN _mipmap_startup USING (utid)
|> SELECT _slice_flattened.*;

-- Span join flattened slices with thread states to get thread state information
-- for each slice, then assemble the per-slice columns. Left join keeps every
-- thread_state region (slice columns null where no flattened slice covers it).
CREATE PERFETTO PIPELINE _mipmap_flat_slice_thread_states_and_slices MATERIALIZED AS
FROM thread_state
|> INTERVAL SPLIT _mipmap_flat_slice AS fs PER utid
|> SELECT
     row_number() OVER () AS id,
     ts,
     dur,
     utid,
     fs.slice_id,
     fs.depth,
     fs.name,
     sched_state_to_human_readable_string(state) AS state,
     cpu,
     io_wait,
     blocked_function,
     coalesce(fs.name, sched_state_to_human_readable_string(state)) AS synth_name,
     irq_context;

-- Intersect the slices with thread states with the unique startup intervals.
-- This table contains the slices that occurred during each startup.
CREATE PERFETTO PIPELINE _mipmap_startup_slice MATERIALIZED AS
SUBPIPELINE valid_slices AS (
  FROM _mipmap_flat_slice_thread_states_and_slices
  |> WHERE dur > -1
)
INTERVAL INTERSECTION OF (valid_slices AS s, _mipmap_startup AS startup) PER utid
|> SELECT
     ts,
     dur,
     s.utid,
     s.slice_id,
     s.depth,
     s.name,
     s.synth_name,
     s.cpu,
     s.state,
     s.io_wait,
     s.blocked_function,
     s.irq_context,
     startup.startup_id,
     startup.upid,
     startup.startup_type,
     startup.package,
     startup.dur AS startup_dur;

-- ------------------------------------------------------------------
-- MIPMAP Generation Call
-- ------------------------------------------------------------------

--
-- Creates 1ms buckets for startup intervals.
--
-- This table uses the `_mipmap_buckets_table` macro to generate a series of
-- 1-ms buckets for each startup. These buckets will be used to
-- aggregate and summarize the startup activity.
CREATE PERFETTO PIPELINE _mipmap_startup_buckets_1ms MATERIALIZED AS
_mipmap_buckets_table!(
  -- Source table for time range
  (SELECT ts, dur, startup_id FROM _mipmap_startup_slice),
  -- Partitioning column
  startup_id,
  -- Bucket duration in nanoseconds
  1e6  -- 1ms buckets
)
|> ORDER BY id;

--
-- Prepares startup slices for mipmapping.
--
-- This table assigns a unique ID and a `group_hash` to each startup slice. The
-- `group_hash` is created from various properties of the slice, such as its
-- name, depth, and thread state. Slices with the same `group_hash` are
-- considered similar and can be merged during the mipmapping process.
CREATE PERFETTO PIPELINE _mipmap_startup_slices_with_ids MATERIALIZED AS
FROM _mipmap_startup_slice
|> SELECT
     row_number() OVER (ORDER BY ts) AS id,
     hash(
       coalesce(name, ''),
       coalesce(state, ''),
       coalesce(depth, ''),
       coalesce(io_wait, ''),
       coalesce(blocked_function, '')
     ) AS group_hash,
     *
|> ORDER BY id;

-- Creates a 1ms resolution mipmap of Android startup slices.
--
-- This table uses the `_mipmap_merged` macro to generate a
-- mipmap of the startup slices. The mipmap provides a summarized view of the
-- startup, with a resolution of 1 ms. The table contains merged slices
-- representing the dominant event in each time bucket.
CREATE PERFETTO PIPELINE _android_startup_mipmap_1ms(
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
) MATERIALIZED AS
_mipmap_merged!(
  _mipmap_startup_slices_with_ids,
  _mipmap_startup_buckets_1ms,
  startup_id,
  1e6  -- 1ms buckets
) AS mm
|> JOIN _mipmap_startup_slices_with_ids AS s USING (id)
|> SELECT
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
     s.blocked_function;
