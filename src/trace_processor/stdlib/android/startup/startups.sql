--
-- Copyright 2019 The Android Open Source Project
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

SELECT IMPORT('common.slices');
SELECT IMPORT('android.process_metadata');

-- All activity startup events.
CREATE TABLE internal_startup_events AS
SELECT
  ts,
  dur,
  ts + dur AS ts_end,
  STR_SPLIT(s.name, ": ", 1) AS package_name
FROM slice s
JOIN process_track t ON s.track_id = t.id
JOIN process USING(upid)
WHERE
  s.name GLOB 'launching: *'
  AND (process.name IS NULL OR process.name = 'system_server');

-- Gather all startup data. Populate by different sdks.
CREATE TABLE internal_all_startups(
  sdk STRING,
  startup_id INTEGER BIGINT,
  ts BIGINT,
  ts_end BIGINT,
  dur BIGINT,
  package STRING,
  startup_type STRING
);

SELECT IMPORT('android.startup.internal_startups_maxsdk28');
SELECT IMPORT('android.startup.internal_startups_minsdk29');
SELECT IMPORT('android.startup.internal_startups_minsdk33');

-- All activity startups in the trace by startup id.
-- Populated by different scripts depending on the platform version/contents.
--
-- @column id           Startup id.
-- @column ts           Timestamp of startup start.
-- @column ts_end       Timestamp of startup end.
-- @column dur          Startup duration.
-- @column package      Package name.
-- @column startup_type Startup type.
CREATE TABLE android_startups AS
SELECT startup_id, ts, ts_end, dur, package, startup_type FROM
internal_all_startups WHERE ( CASE
  WHEN SLICE_COUNT('launchingActivity#*:*') > 0
    THEN sdk = "minsdk33"
  WHEN SLICE_COUNT('MetricsLogger:*') > 0
    THEN sdk = "minsdk29"
  ELSE sdk = "maxsdk28"
  END);

--
-- Create startup processes
--

-- Create a table containing only the slices which are necessary for determining
-- whether a startup happened.
CREATE TABLE internal_startup_indicator_slices AS
SELECT ts, name, track_id
FROM slice
WHERE name IN ('bindApplication', 'activityStart', 'activityResume');

SELECT CREATE_FUNCTION(
  'INTERNAL_STARTUP_INDICATOR_SLICE_COUNT(start_ts LONG, end_ts LONG, utid INT, name STRING)',
  'INT',
  '
    SELECT COUNT(1)
    FROM thread_track t
    JOIN internal_startup_indicator_slices s ON s.track_id = t.id
    WHERE
      t.utid = $utid AND
      s.ts >= $start_ts AND
      s.ts < $end_ts AND
      s.name = $name
  '
);

-- Maps a startup to the set of processes that handled the activity start.
--
-- The vast majority of cases should be a single process. However it is
-- possible that the process dies during the activity startup and is respawned.
--
-- @column startup_id   Startup id.
-- @column upid         Upid of process on which activity started.
-- @column startup_type Type of the startup.
CREATE TABLE android_startup_processes AS
-- This is intentionally a materialized query. For some reason, if we don't
-- materialize, we end up with a query which is an order of magnitude slower :(
WITH startup_with_type AS MATERIALIZED (
  SELECT
    startup_id,
    upid,
    CASE
      -- type parsed from platform event takes precedence if available
      WHEN startup_type IS NOT NULL THEN startup_type
      WHEN bind_app > 0 AND a_start > 0 AND a_resume > 0 THEN 'cold'
      WHEN a_start > 0 AND a_resume > 0 THEN 'warm'
      WHEN a_resume > 0 THEN 'hot'
      ELSE NULL
    END AS startup_type
  FROM (
    SELECT
      l.startup_id,
      l.startup_type,
      p.upid,
      INTERNAL_STARTUP_INDICATOR_SLICE_COUNT(l.ts, l.ts_end, t.utid, 'bindApplication') AS bind_app,
      INTERNAL_STARTUP_INDICATOR_SLICE_COUNT(l.ts, l.ts_end, t.utid, 'activityStart') AS a_start,
      INTERNAL_STARTUP_INDICATOR_SLICE_COUNT(l.ts, l.ts_end, t.utid, 'activityResume') AS a_resume
    FROM android_startups l
    JOIN android_process_metadata p ON (
      l.package = p.package_name
      -- If the package list data source was not enabled in the trace, nothing
      -- will match the above constraint so also match any process whose name
      -- is a prefix of the package name.
      OR (
        (SELECT COUNT(1) = 0 FROM package_list)
        AND p.process_name GLOB l.package || '*'
      )
      )
    JOIN thread t ON (p.upid = t.upid AND t.is_main_thread)
  )
)
SELECT *
FROM startup_with_type
WHERE startup_type IS NOT NULL;


-- Maps a startup to the set of threads on processes that handled the
-- activity start.
--
-- @column startup_id     Startup id.
-- @column ts             Timestamp of start.
-- @column dur            Duration of startup.
-- @column upid           Upid of process involved in startup.
-- @column utid           Utid of the thread.
-- @column thread_name    Name of the thread.
-- @column is_main_thread Thread is a main thread.
CREATE VIEW android_startup_threads AS
SELECT
  startups.startup_id,
  startups.ts,
  startups.dur,
  android_startup_processes.upid,
  thread.utid,
  thread.name AS thread_name,
  thread.is_main_thread AS is_main_thread
FROM android_startups startups
JOIN android_startup_processes USING (startup_id)
JOIN thread USING (upid);

---
--- Functions
---

-- All the slices for all startups in trace.
--
-- Generally, this view should not be used. Instead, use one of the view functions related
-- to the startup slices which are created from this table.
--
-- @column startup_ts     Timestamp of startup.
-- @column startup_ts_end Timestamp of startup end.
-- @column startup_id     Startup id.
-- @column utid           UTID of thread with slice.
-- @column thread_name    Name of thread.
-- @column is_main_thread Whether it is main thread.
-- @column arg_set_id     Arg set id.
-- @column slice_id       Slice id.
-- @column slice_name     Name of slice.
-- @column slice_ts       Timestamp of slice start.
-- @column slice_dur      Slice duration.
CREATE VIEW android_thread_slices_for_all_startups AS
SELECT
  st.ts AS startup_ts,
  st.ts + st.dur AS startup_ts_end,
  st.startup_id,
  st.utid,
  st.thread_name,
  st.is_main_thread,
  slice.arg_set_id,
  slice.id as slice_id,
  slice.name AS slice_name,
  slice.ts AS slice_ts,
  slice.dur AS slice_dur
FROM android_startup_threads st
JOIN thread_track USING (utid)
JOIN slice ON (slice.track_id = thread_track.id)
WHERE slice.ts BETWEEN st.ts AND st.ts + st.dur;

-- Given a startup id and GLOB for a slice name, returns matching slices with data.
--
-- @arg startup_id INT    Startup id.
-- @arg slice_name STRING Glob of the slice.
-- @column slice_name     Name of the slice.
-- @column slice_ts       Timestamp of start of the slice.
-- @column slice_dur      Duration of the slice.
-- @column thread_name    Name of the thread with the slice.
-- @column arg_set_id     Arg set id.
SELECT CREATE_VIEW_FUNCTION(
  'ANDROID_SLICES_FOR_STARTUP_AND_SLICE_NAME(startup_id INT, slice_name STRING)',
  'slice_name STRING, slice_ts INT, slice_dur INT, thread_name STRING, arg_set_id INT',
  '
    SELECT slice_name, slice_ts, slice_dur, thread_name, arg_set_id
    FROM android_thread_slices_for_all_startups
    WHERE startup_id = $startup_id AND slice_name GLOB $slice_name
  '
);

-- Returns binder transaction slices for a given startup id with duration over threshold.
--
-- @arg startup_id INT    Startup id.
-- @arg threshold DOUBLE  Only return slices with duration over threshold.
-- @column id             Slice id.
-- @column slice_dur      Slice duration.
-- @column thread_name    Name of the thread with slice.
-- @column process        Name of the process with slice.
-- @column arg_set_id     Arg set id.
-- @column is_main_thread Whether is main thread.
SELECT CREATE_VIEW_FUNCTION(
  'ANDROID_BINDER_TRANSACTION_SLICES_FOR_STARTUP(startup_id INT, threshold DOUBLE)',
  'id INT, slice_dur INT, thread_name STRING, process STRING, arg_set_id INT, is_main_thread BOOL',
  '
    SELECT slice_id as id, slice_dur, thread_name, process.name as process, s.arg_set_id, is_main_thread
    FROM android_thread_slices_for_all_startups s
    JOIN process ON (
      EXTRACT_ARG(s.arg_set_id, "destination process") = process.pid
    )
    WHERE startup_id = $startup_id AND slice_name GLOB "binder transaction" AND slice_dur > $threshold
  '
);

-- Returns duration of startup for slice name.
--
-- Sums duration of all slices of startup with provided name.
--
-- @arg startup_id LONG   Startup id.
-- @arg slice_name STRING Slice name.
-- @ret INT               Sum of duration.
SELECT CREATE_FUNCTION(
  'ANDROID_SUM_DUR_FOR_STARTUP_AND_SLICE(startup_id LONG, slice_name STRING)',
  'INT',
  '
    SELECT SUM(slice_dur)
    FROM android_thread_slices_for_all_startups
    WHERE startup_id = $startup_id AND slice_name GLOB $slice_name
  '
);

-- Returns duration of startup for slice name on main thread.
--
-- Sums duration of all slices of startup with provided name only on main thread.
--
-- @arg startup_id LONG   Startup id.
-- @arg slice_name STRING Slice name.
-- @ret INT               Sum of duration.
SELECT CREATE_FUNCTION(
  'ANDROID_SUM_DUR_ON_MAIN_THREAD_FOR_STARTUP_AND_SLICE(startup_id LONG, slice_name STRING)',
  'INT',
  '
    SELECT SUM(slice_dur)
    FROM android_thread_slices_for_all_startups
    WHERE startup_id = $startup_id AND slice_name GLOB $slice_name AND is_main_thread
  '
);