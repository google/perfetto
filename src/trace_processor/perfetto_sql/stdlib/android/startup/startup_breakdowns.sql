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
CREATE PERFETTO TABLE _startup_root_slices AS
WITH
  possibly_overlapping AS (
    -- There's a bug (b/456092940) where we can have concurrent startups with
    -- the same upid. So we pre-filter to pick one in any concurrent group.
    SELECT
      (SELECT max(id) FROM slice) + row_number() OVER () AS id,
      android_startups.dur AS dur,
      android_startups.ts AS ts,
      android_startups.startup_id,
      android_startups.startup_type,
      process.name AS process_name,
      thread.utid AS utid
    FROM android_startup_processes AS startup
    JOIN android_startups USING (startup_id)
    JOIN thread
      ON thread.upid = process.upid
      AND thread.is_main_thread
    JOIN process
      ON process.upid = startup.upid
    WHERE
      android_startups.dur > 0
    ORDER BY
      ts
  ),
  unique_startups AS (
    -- The following self interval intersect will only yield |count| > 1 when
    -- we have concurrent startups on the same utid. Filtering out the |count| > 1
    -- leaves us with non concurrent startups per utid.
    SELECT id_0 AS id, count() AS count
    FROM _interval_intersect!(
      (possibly_overlapping, possibly_overlapping),
      (utid)
    )
    GROUP BY
      utid,
      ts
    HAVING
      count = 1
  )
SELECT possibly_overlapping.*
FROM possibly_overlapping
JOIN unique_startups USING (id);

-- Startup slices overlapping a startup on its main thread, normalized with
-- _normalize_android_string. Materialized once because it is referenced
-- multiple times below (and inside _slice_remove_nulls_and_reparent), and the
-- overlap join against the thread_slice view is expensive to re-evaluate.
CREATE PERFETTO TABLE _startup_relevant_slices AS
SELECT
  slice.id,
  slice.parent_id,
  slice.depth,
  _normalize_android_string(slice.name) AS name,
  slice.ts,
  slice.dur,
  slice.utid
FROM thread_slice AS slice
JOIN _startup_root_slices AS startup
  ON slice.utid = startup.utid
  AND max(slice.ts, startup.ts)
  < min(slice.ts + slice.dur, startup.ts + startup.dur)
WHERE
  slice.dur > 0;

-- All relevant startup slices normalized with _normalize_android_string.
CREATE PERFETTO TABLE _startup_normalized_slices AS
SELECT p.id, p.parent_id, p.depth, p.name, s.ts, s.dur, s.utid
FROM _slice_remove_nulls_and_reparent
    !(
      (SELECT id, parent_id, depth, name FROM _startup_relevant_slices),
      name) AS p
JOIN _startup_relevant_slices AS s USING (id)
ORDER BY
  p.id;

-- Flattened slice stack of each startup's main thread: at every point of
-- the startup, the leaf (i.e. deepest) normalized slice, or a NULL slice_id
-- where no normalized slice is active.
CREATE PERFETTO TABLE _startup_flat_slices AS
INTERVAL INTERSECTION OF (
  _startup_normalized_slices AS s,
  _startup_root_slices AS r
) PER utid
|> SELECT ts, dur, utid, s.id, r.id AS root_id
-- TODO(lalitm): MAX(id) stands in for the deepest slice and relies on a
-- child's id being greater than its parent's, which is not guaranteed.
-- Switch to ARG_MAX(id, depth) once pipelines support it.
|> INTERVAL FLATTEN PER root_id, utid AGGREGATE MAX(id) AS slice_id
|> INTERVAL FILL GAPS WITH (
  SELECT id AS root_id, ts, dur, utid FROM _startup_root_slices
) AS r PER root_id;

-- Intersection of the main thread's thread states with
-- _startup_flat_slices: each row has a single thread state and leaf slice.
CREATE PERFETTO TABLE _startup_thread_states_and_slices_breakdown AS
INTERVAL INTERSECTION OF (
  (
    -- Thread states still open at the end of the trace have dur = -1.
    SELECT
      id,
      ts,
      iif(dur = -1, trace_end() - ts, dur) AS dur,
      utid,
      state,
      io_wait,
      irq_context
    FROM thread_state
    WHERE
      dur != 0
      AND utid IN (SELECT utid FROM _startup_root_slices)
  ) AS t,
  _startup_flat_slices AS f
) PER utid
|> SELECT
  ts,
  dur,
  f.root_id,
  f.slice_id,
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
CREATE PERFETTO TABLE android_startup_opinionated_breakdown(
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
AS
SELECT
  b.ts,
  b.dur,
  startup.startup_id,
  b.slice_id,
  b.thread_state_id,
  _startup_breakdown_reason(s.name, b.state, b.io_wait, b.irq_context) AS reason
FROM _startup_thread_states_and_slices_breakdown AS b
JOIN _startup_root_slices AS startup
  ON startup.id = b.root_id
LEFT JOIN _startup_normalized_slices AS s
  ON s.id = b.slice_id
UNION ALL
-- Augment the existing startup breakdown with an artificial slice accounting for
-- any launch delays before the app starts handling startup on its main thread
SELECT
  startup.ts,
  min(b.ts) - startup.ts AS dur,
  startup.startup_id,
  NULL AS slice_id,
  NULL AS thread_state_id,
  'launch_delay' AS reason
FROM _startup_thread_states_and_slices_breakdown AS b
JOIN _startup_root_slices AS startup
  ON startup.id = b.root_id
GROUP BY
  b.root_id
HAVING
  min(b.ts) - startup.ts > 0;
