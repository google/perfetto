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

INCLUDE PERFETTO MODULE android.startup.startups;

INCLUDE PERFETTO MODULE intervals.overlap;

INCLUDE PERFETTO MODULE slices.hierarchy;

INCLUDE PERFETTO MODULE slices.with_context;

INCLUDE PERFETTO MODULE intervals.intersect;

-- Maps slice names with common prefixes to a static string key.
-- Returns NULL if there's no mapping.
CREATE PERFETTO FUNCTION _normalize_android_string(name STRING)
RETURNS STRING
AS
SELECT
  CASE
    WHEN $name = 'mm_vmscan_direct_reclaim' THEN 'kernel_memory_reclaim'
    WHEN $name GLOB 'GC: Wait For*' THEN 'userspace_memory_reclaim'
    WHEN ($name GLOB 'monitor contention*'
    OR $name GLOB 'Lock contention on a monitor lock*') THEN 'monitor_contention'
    WHEN $name GLOB 'Lock contention*' THEN 'art_lock_contention'
    WHEN ($name = 'binder transaction' OR $name = 'binder reply') THEN 'binder'
    WHEN $name = 'Contending for pthread mutex' THEN 'mutex_contention'
    WHEN $name GLOB 'dlopen*' THEN 'dlopen'
    WHEN $name GLOB 'VerifyClass*' THEN 'verify_class'
    WHEN $name = 'inflate' THEN 'inflate'
    WHEN $name GLOB 'Choreographer#doFrame*' THEN 'choreographer_do_frame'
    WHEN $name GLOB 'OpenDexFilesFromOat*' THEN 'open_dex_files_from_oat'
    WHEN $name = 'ResourcesManager#getResources' THEN 'resources_manager_get_resources'
    WHEN $name = 'bindApplication' THEN 'bind_application'
    WHEN $name = 'activityStart' THEN 'activity_start'
    WHEN $name = 'activityResume' THEN 'activity_resume'
    WHEN $name = 'activityRestart' THEN 'activity_restart'
    WHEN $name = 'clientTransactionExecuted' THEN 'client_transaction_executed'
    ELSE NULL
  END AS name;

-- Derives a startup reason from a slice name and some thread_state columns.
CREATE PERFETTO FUNCTION _startup_breakdown_reason(
  name STRING,
  state STRING,
  io_wait LONG,
  irq_context LONG
)
RETURNS STRING
AS
SELECT
  CASE
    WHEN $io_wait = 1 THEN 'io'
    WHEN $name IS NOT NULL THEN $name
    WHEN $irq_context = 1 THEN 'irq'
    ELSE $state
  END AS name;

-- List of startups with unique ids for each possible upid. The existing
-- startup_ids are not necessarily unique (because of multiuser).
CREATE PERFETTO PIPELINE _startup_root_slices MATERIALIZED AS
-- There's a bug (b/456092940) where we can have concurrent startups with
-- the same upid. So we pre-filter to pick one in any concurrent group.
SUBPIPELINE possibly_overlapping AS (
  FROM android_startup_processes AS startup
  |> JOIN android_startups USING (startup_id)
  |> JOIN process ON process.upid = startup.upid
  |> JOIN thread ON thread.upid = process.upid AND thread.is_main_thread
  |> WHERE android_startups.dur > 0
  |> SELECT
    (SELECT max(id) FROM slice) + row_number() OVER () AS id,
    android_startups.dur AS dur,
    android_startups.ts AS ts,
    android_startups.startup_id,
    android_startups.startup_type,
    process.name AS process_name,
    thread.utid AS utid
  |> ORDER BY ts
)
-- The following self interval intersect will only yield |count| > 1 when
-- we have concurrent startups on the same utid. Filtering out the |count| > 1
-- leaves us with non concurrent startups per utid.
SUBPIPELINE unique_startups AS (
  INTERVAL INTERSECTION OF (
    possibly_overlapping AS a,
    possibly_overlapping AS b
  ) PER utid
  |> AGGREGATE ANY_VALUE(a.id) AS id, count() AS count GROUP BY utid, ts
  |> WHERE count = 1
)
FROM possibly_overlapping
|> JOIN unique_startups USING (id)
|> SELECT possibly_overlapping.*;

-- All relevant startup slices normalized with _normalize_android_string.
-- The null-named slices are removed and their children reparented to the nearest
-- surviving ancestor via TREE CONTRACT.
CREATE PERFETTO PIPELINE _startup_normalized_slices MATERIALIZED AS
SUBPIPELINE relevant_startup_slices AS (
  -- Keep only thread slices overlapping a startup root on the same thread.
  -- Nothing from the startup root is read, so this is a temporal semijoin.
  FROM thread_slice AS slice
  |> WHERE slice.dur > 0
  |> INTERVAL WHERE OVERLAPPING BOUNDS _startup_root_slices PER utid
  |> SELECT
    slice.id,
    slice.parent_id,
    slice.depth,
    _normalize_android_string(slice.name) AS name
)
FROM relevant_startup_slices
|> TREE CONTRACT AT (
  FROM relevant_startup_slices |> WHERE name IS NULL |> SELECT id
) OVER relevant_startup_slices
|> JOIN thread_slice USING (id)
|> SELECT
  id,
  parent_id,
  depth,
  name,
  thread_slice.ts,
  thread_slice.dur,
  thread_slice.utid
|> ORDER BY id;

-- Subset of _startup_normalized_slices that occurred during any app startups on the main thread.
-- Their timestamps and durations are chopped to fit within the respective app startup duration.
CREATE PERFETTO PIPELINE _startup_slices_breakdown MATERIALIZED AS
-- Derive the root/child relationship between the startup roots and the
-- normalized slices via an interval intersection on utid, then re-anchor
-- parent_id and root_id from the intersected operands.
INTERVAL INTERSECTION OF (
  _startup_normalized_slices AS c,
  _startup_root_slices AS r
) PER utid
|> WHERE c.dur > 0 AND r.dur > 0
|> SELECT
  ts,
  dur,
  c.id,
  coalesce(c.parent_id, r.id) AS parent_id,
  r.id AS root_id,
  r.ts AS root_ts,
  r.dur AS root_dur,
  utid;

-- Flattened slice version of _startup_slices_breakdown. This selects the leaf slice at every region
-- of the slice stack.
CREATE PERFETTO PIPELINE _startup_flat_slices_breakdown MATERIALIZED AS
FROM _startup_slices_breakdown
|> INTERVAL FLATTEN PER root_id AGGREGATE ARG_MAX(dur, id) AS id
|> JOIN _startup_normalized_slices AS s USING (id)
|> SELECT ts, dur, root_id, s.id AS slice_id, s.name;

-- Subset of thread_states that occurred during any app startups on the main thread.
CREATE PERFETTO PIPELINE _startup_thread_states_breakdown MATERIALIZED AS
SUBPIPELINE states AS (
  FROM thread_state
  |> SELECT *, NULL AS parent_id
)
INTERVAL INTERSECTION OF (
  states AS c,
  _startup_root_slices AS r
) PER utid
|> WHERE c.dur > 0 AND r.dur > 0
|> JOIN thread_state AS t ON t.id = c.id
|> SELECT
  ts,
  dur,
  r.id AS root_id,
  t.id AS thread_state_id,
  t.state,
  t.io_wait,
  t.irq_context;

-- Blended thread state and slice breakdown blocking app startups.
--
-- Each row blames a unique period during an app startup with a reason
-- derived from the slices and thread states on the main thread.
--
-- Some helpful events to enables are binder transactions, ART, am and view.
CREATE PERFETTO PIPELINE android_startup_opinionated_breakdown(
  -- Startup id.
  startup_id JOINID(android_startups.startup_id),
  -- Id of relevant slice blocking startup.
  slice_id JOINID(slice.id),
  -- Id of thread_state blocking startup.
  thread_state_id JOINID(thread_state.id),
  -- Timestamp of an exclusive interval during the app startup with a single latency reason.
  ts TIMESTAMP,
  -- Duration of an exclusive interval during the app startup with a single latency reason.
  dur DURATION,
  -- Cause of delay during an exclusive interval of the app startup.
  reason STRING
)
MATERIALIZED AS
-- Intersection of _startup_flat_slices_breakdown and
-- _startup_thread_states_breakdown. A left intersection is used since some
-- parts of the slice stack may not have any slices but will have thread
-- states: the thread states are split at the flat slice boundaries with
-- INTERVAL SPLIT (null slice columns where no slice is present).
FROM _startup_thread_states_breakdown AS b
|> INTERVAL SPLIT _startup_flat_slices_breakdown AS s PER root_id
|> JOIN _startup_root_slices AS startup ON startup.id = b.root_id
|> SELECT
  b.ts,
  b.dur,
  startup.startup_id,
  s.slice_id,
  b.thread_state_id,
  _startup_breakdown_reason(s.name, b.state, b.io_wait, b.irq_context) AS reason
|> UNION ALL (
  -- Augment the existing startup breakdown with an artificial slice accounting
  -- for any launch delays before the app starts handling startup on its main
  -- thread.
  FROM _startup_thread_states_breakdown
  |> JOIN _startup_root_slices ON _startup_root_slices.id = root_id
  |> AGGREGATE
    ANY_VALUE(_startup_root_slices.ts) AS ts,
    min(_startup_thread_states_breakdown.ts) - ANY_VALUE(_startup_root_slices.ts) AS dur,
    ANY_VALUE(startup_id) AS startup_id
    GROUP BY root_id
  |> WHERE dur > 0
  |> SELECT
    startup_id,
    NULL AS slice_id,
    NULL AS thread_state_id,
    ts,
    dur,
    'launch_delay' AS reason
);
