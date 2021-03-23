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
--

-- Create the base tables and views containing the launch spans.
SELECT RUN_METRIC('android/android_startup_launches.sql');
SELECT RUN_METRIC('android/process_metadata.sql');
SELECT RUN_METRIC('android/hsc_startups.sql');

-- Slices for forked processes. Never present in hot starts.
-- Prefer this over process start_ts, since the process might have
-- been preforked.
DROP VIEW IF EXISTS zygote_fork_slice;
CREATE VIEW zygote_fork_slice AS
SELECT slice.ts, slice.dur, STR_SPLIT(slice.name, ": ", 1) AS process_name
FROM slice WHERE name LIKE 'Start proc: %';

DROP TABLE IF EXISTS zygote_forks_by_id;
CREATE TABLE zygote_forks_by_id AS
SELECT
  launches.id,
  zygote_fork_slice.ts,
  zygote_fork_slice.dur
FROM zygote_fork_slice
JOIN launches
ON (launches.ts < zygote_fork_slice.ts
    AND zygote_fork_slice.ts + zygote_fork_slice.dur < launches.ts_end
    AND zygote_fork_slice.process_name = launches.package
);

DROP VIEW IF EXISTS launch_main_threads;
CREATE VIEW launch_main_threads AS
SELECT
  launches.ts AS ts,
  launches.dur AS dur,
  launches.id AS launch_id,
  thread.utid AS utid
FROM launches
JOIN launch_processes ON launches.id = launch_processes.launch_id
JOIN process USING(upid)
JOIN thread ON (process.upid = thread.upid AND process.pid = thread.tid)
ORDER BY ts;

DROP VIEW IF EXISTS thread_state_extended;
CREATE VIEW thread_state_extended AS
SELECT
  ts,
  IIF(dur = -1, (SELECT end_ts FROM trace_bounds), dur) AS dur,
  utid,
  state
FROM thread_state;

DROP TABLE IF EXISTS main_thread_state;
CREATE VIRTUAL TABLE main_thread_state
USING SPAN_JOIN(
  launch_main_threads PARTITIONED utid,
  thread_state_extended PARTITIONED utid);

DROP VIEW IF EXISTS launch_by_thread_state;
CREATE VIEW launch_by_thread_state AS
SELECT launch_id, state, SUM(dur) AS dur
FROM main_thread_state
GROUP BY 1, 2;

-- Tracks all slices for the main process threads
DROP TABLE IF EXISTS main_process_slice;
CREATE TABLE main_process_slice AS
SELECT
  launches.id AS launch_id,
  CASE
    WHEN slice.name LIKE 'OpenDexFilesFromOat%' THEN 'OpenDexFilesFromOat'
    WHEN slice.name LIKE 'VerifyClass%' THEN 'VerifyClass'
    ELSE slice.name
  END AS name,
  AndroidStartupMetric_Slice(
    'dur_ns', SUM(slice.dur),
    'dur_ms', SUM(slice.dur) / 1e6
  ) AS slice_proto
FROM launches
JOIN launch_processes ON (launches.id = launch_processes.launch_id)
JOIN thread ON (launch_processes.upid = thread.upid)
JOIN thread_track USING (utid)
JOIN slice ON (
  slice.track_id = thread_track.id
  AND slice.ts BETWEEN launches.ts AND launches.ts + launches.dur)
WHERE slice.name IN (
  'PostFork',
  'ActivityThreadMain',
  'bindApplication',
  'activityStart',
  'activityRestart',
  'activityResume',
  'inflate',
  'ResourcesManager#getResources')
  OR slice.name LIKE 'performResume:%'
  OR slice.name LIKE 'performCreate:%'
  OR slice.name LIKE 'location=% status=% filter=% reason=%'
  OR slice.name LIKE 'OpenDexFilesFromOat%'
  OR slice.name LIKE 'VerifyClass%'
  OR slice.name LIKE 'Choreographer#doFrame%'
GROUP BY 1, 2;

DROP TABLE IF EXISTS report_fully_drawn_per_launch;
CREATE TABLE report_fully_drawn_per_launch AS
WITH report_fully_drawn_launch_slices AS (
  SELECT
    launches.id AS launch_id,
    launches.ts AS launch_ts,
    min(slice.ts) as report_fully_drawn_ts
  FROM launches
  JOIN launch_processes ON (launches.id = launch_processes.launch_id)
  JOIN thread ON (launch_processes.upid = thread.upid)
  JOIN thread_track USING (utid)
  JOIN slice ON (
    slice.track_id = thread_track.id
    AND slice.ts >= launches.ts)
  WHERE slice.name LIKE 'reportFullyDrawn%'
  GROUP BY launches.id
)
SELECT
  launch_id,
  report_fully_drawn_ts - launch_ts as report_fully_drawn_dur
FROM report_fully_drawn_launch_slices;

DROP VIEW IF EXISTS to_event_protos;
CREATE VIEW to_event_protos AS
SELECT
  slice.name as slice_name,
  launch_id,
  AndroidStartupMetric_Slice(
    'dur_ns', slice.ts - l.ts,
    'dur_ms', (slice.ts - l.ts) / 1e6
  ) as slice_proto
FROM launch_main_threads l
JOIN thread_track USING (utid)
JOIN slice ON (
  slice.track_id = thread_track.id
  AND slice.ts BETWEEN l.ts AND l.ts + l.dur);

DROP VIEW IF EXISTS startup_view;
CREATE VIEW startup_view AS
SELECT
  AndroidStartupMetric_Startup(
    'startup_id', launches.id,
    'package_name', launches.package,
    'process_name', (
      SELECT name FROM process
      WHERE upid IN (
        SELECT upid FROM launch_processes p
        WHERE p.launch_id = launches.id
        LIMIT 1
      )
    ),
    'process', (
      SELECT metadata FROM process_metadata
      WHERE upid IN (
        SELECT upid FROM launch_processes p
        WHERE p.launch_id = launches.id
        LIMIT 1
      )
    ),
    'activities', (
      SELECT RepeatedField(AndroidStartupMetric_Activity(
        'name', (SELECT STR_SPLIT(s.name, ':', 1)),
        'method', (SELECT STR_SPLIT(s.name, ':', 0)),
        'slice', s.slice_proto
      ))
      FROM main_process_slice s
      WHERE s.launch_id = launches.id
      AND (name LIKE 'performResume:%' OR name LIKE 'performCreate:%')
    ),
    'zygote_new_process', EXISTS(SELECT TRUE FROM zygote_forks_by_id WHERE id = launches.id),
    'activity_hosting_process_count', (
      SELECT COUNT(1) FROM launch_processes p
      WHERE p.launch_id = launches.id
    ),
    'to_first_frame', AndroidStartupMetric_ToFirstFrame(
      'dur_ns', launches.dur,
      'dur_ms', launches.dur / 1e6,
      'main_thread_by_task_state', AndroidStartupMetric_TaskStateBreakdown(
        'running_dur_ns', IFNULL(
            (
            SELECT dur FROM launch_by_thread_state l
            WHERE l.launch_id = launches.id AND state = 'Running'
            ), 0),
        'runnable_dur_ns', IFNULL(
            (
            SELECT dur FROM launch_by_thread_state l
            WHERE l.launch_id = launches.id AND state = 'R'
            ), 0),
        'uninterruptible_sleep_dur_ns', IFNULL(
            (
            SELECT dur FROM launch_by_thread_state l
            WHERE l.launch_id = launches.id AND (state = 'D' or state = 'DK')
            ), 0),
        'interruptible_sleep_dur_ns', IFNULL(
            (
            SELECT dur FROM launch_by_thread_state l
            WHERE l.launch_id = launches.id AND state = 'S'
            ), 0)
      ),
      'to_post_fork', (
        SELECT slice_proto
        FROM to_event_protos p
        WHERE p.launch_id = launches.id AND slice_name = 'PostFork'
      ),
      'to_activity_thread_main', (
        SELECT slice_proto
        FROM to_event_protos p
        WHERE p.launch_id = launches.id AND slice_name = 'ActivityThreadMain'
      ),
      'to_bind_application', (
        SELECT slice_proto
        FROM to_event_protos p
        WHERE p.launch_id = launches.id AND slice_name = 'bindApplication'
      ),
      'other_processes_spawned_count', (
        SELECT COUNT(1) FROM process
        WHERE (process.name IS NULL OR process.name != launches.package)
        AND process.start_ts BETWEEN launches.ts AND launches.ts + launches.dur
      ),
      'time_activity_manager', (
        SELECT AndroidStartupMetric_Slice(
          'dur_ns', l.ts - launches.ts,
          'dur_ms', (l.ts - launches.ts) / 1e6
        )
        FROM launching_events l
        WHERE l.ts BETWEEN launches.ts AND launches.ts + launches.dur
      ),
      'time_post_fork', (
        SELECT slice_proto
        FROM main_process_slice s
        WHERE s.launch_id = launches.id AND name = 'PostFork'
      ),
      'time_activity_thread_main', (
        SELECT slice_proto
        FROM main_process_slice s
        WHERE s.launch_id = launches.id AND name = 'ActivityThreadMain'
      ),
      'time_bind_application', (
        SELECT slice_proto
        FROM main_process_slice s
        WHERE s.launch_id = launches.id AND name = 'bindApplication'
      ),
      'time_activity_start', (
        SELECT slice_proto
        FROM main_process_slice s
        WHERE s.launch_id = launches.id AND name = 'activityStart'
      ),
      'time_activity_resume', (
        SELECT slice_proto
        FROM main_process_slice s
        WHERE s.launch_id = launches.id AND name = 'activityResume'
      ),
      'time_activity_restart', (
        SELECT slice_proto
        FROM main_process_slice s
        WHERE s.launch_id = launches.id AND name = 'activityRestart'
      ),
      'time_choreographer', (
        SELECT slice_proto
        FROM main_process_slice s
        WHERE s.launch_id = launches.id AND name LIKE 'Choreographer#doFrame%'
      ),
      'time_before_start_process', (
        SELECT AndroidStartupMetric_Slice(
          'dur_ns', ts - launches.ts,
          'dur_ms', (ts - launches.ts) / 1e6
        )
        FROM zygote_forks_by_id z
        WHERE z.id = launches.id
      ),
      'time_during_start_process', (
        SELECT AndroidStartupMetric_Slice(
          'dur_ns', dur,
          'dur_ms', dur / 1e6
        )
        FROM zygote_forks_by_id z
        WHERE z.id = launches.id
      ),
      'time_inflate', (
        SELECT slice_proto
        FROM main_process_slice s
        WHERE s.launch_id = launches.id AND name = 'inflate'
      ),
      'time_get_resources', (
        SELECT slice_proto
        FROM main_process_slice s
        WHERE s.launch_id = launches.id
        AND name = 'ResourcesManager#getResources'
      ),
      'time_dex_open', (
        SELECT slice_proto
        FROM main_process_slice s
        WHERE s.launch_id = launches.id AND name = 'OpenDexFilesFromOat'
      ),
      'time_verify_class', (
        SELECT slice_proto
        FROM main_process_slice s
        WHERE s.launch_id = launches.id AND name = 'VerifyClass'
      )
    ),
    'hsc', (
      SELECT NULL_IF_EMPTY(AndroidStartupMetric_HscMetrics(
        'full_startup', (
          SELECT AndroidStartupMetric_Slice(
            'dur_ns', h.ts_total,
            'dur_ms', h.ts_total / 1e6
          )
          FROM hsc_based_startup_times h
          WHERE h.id = launches.id
        )
      ))
    ),
    'report_fully_drawn', (
      SELECT NULL_IF_EMPTY(AndroidStartupMetric_Slice(
        'dur_ns', report_fully_drawn_dur,
        'dur_ms', report_fully_drawn_dur / 1e6
      ))
      FROM report_fully_drawn_per_launch r
      WHERE r.launch_id = launches.id
    ),
    'optimization_status',(
      SELECT RepeatedField(AndroidStartupMetric_OptimizationStatus(
        'location', SUBSTR(STR_SPLIT(name, ' status=', 0), LENGTH('location=') + 1),
        'odex_status', STR_SPLIT(STR_SPLIT(name, ' status=', 1), ' filter=', 0),
        'compilation_filter', STR_SPLIT(STR_SPLIT(name, ' filter=', 1), ' reason=', 0),
        'compilation_reason', STR_SPLIT(name, ' reason=', 1)
      ))
      FROM main_process_slice s
      WHERE name LIKE 'location=% status=% filter=% reason=%'
    )
  ) as startup
FROM launches;

DROP VIEW IF EXISTS android_startup_event;
CREATE VIEW android_startup_event AS
SELECT
  'slice' as track_type,
  'Android App Startups' as track_name,
  l.ts as ts,
  l.dur as dur,
  l.package as slice_name
FROM launches l;

DROP VIEW IF EXISTS android_startup_output;
CREATE VIEW android_startup_output AS
SELECT
  AndroidStartupMetric(
    'startup', (
      SELECT RepeatedField(startup) FROM startup_view
    )
  );
