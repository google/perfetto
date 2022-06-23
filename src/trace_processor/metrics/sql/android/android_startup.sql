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
SELECT RUN_METRIC('android/startup/launches.sql');

-- Define the helper functions which will be used throught the remainder
-- of the metric.
SELECT RUN_METRIC('android/startup/slice_functions.sql');

-- Run all the HSC metrics.
SELECT RUN_METRIC('android/startup/hsc.sql');

-- Define some helper functions related to breaking down thread state
-- for launches.
SELECT RUN_METRIC('android/startup/thread_state_breakdown.sql');

-- Define helper functions to break down slices/threads by thread
-- state.
SELECT RUN_METRIC('android/startup/mcycles_per_launch.sql');

-- Define helper functions for GC slices.
SELECT RUN_METRIC('android/startup/gc_slices.sql');

-- Define helper functions for system state.
SELECT RUN_METRIC('android/startup/system_state.sql');

-- Define process metadata functions.
SELECT RUN_METRIC('android/process_metadata.sql');

-- Returns the slices for forked processes. Never present in hot starts.
-- Prefer this over process start_ts, since the process might have
-- been preforked.
SELECT CREATE_VIEW_FUNCTION(
  'ZYGOTE_FORK_FOR_LAUNCH(launch_id INT)',
  'ts INT, dur INT',
  '
    SELECT slice.ts, slice.dur
    FROM launches
    JOIN slice ON (
      launches.ts < slice.ts AND
      slice.ts + slice.dur < launches.ts_end AND
      STR_SPLIT(slice.name, ": ", 1) = launches.package
    )
    WHERE launches.id = $launch_id AND slice.name GLOB "Start proc: *"
  '
);

-- Returns the fully drawn slice proto given a launch id.
SELECT CREATE_FUNCTION(
  'REPORT_FULLY_DRAWN_FOR_LAUNCH(launch_id INT)',
  'PROTO',
  '
    SELECT
      STARTUP_SLICE_PROTO(report_fully_drawn_ts - launch_ts)
    FROM (
      SELECT
        launches.ts AS launch_ts,
        min(slice.ts) AS report_fully_drawn_ts
      FROM launches
      JOIN launch_processes ON (launches.id = launch_processes.launch_id)
      JOIN thread USING (upid)
      JOIN thread_track USING (utid)
      JOIN slice ON (slice.track_id = thread_track.id)
      WHERE
        slice.name GLOB "reportFullyDrawn*" AND
        slice.ts >= launches.ts AND
        launches.id = $launch_id
    )
  '
);

-- Returns a repeated field of all long binder transaction protos for
-- a given launch id.
SELECT CREATE_FUNCTION(
  'BINDER_TRANSACTION_PROTO_FOR_LAUNCH(launch_id INT)',
  'PROTO',
  '
    SELECT RepeatedField(
      AndroidStartupMetric_BinderTransaction(
        "duration", STARTUP_SLICE_PROTO(s.slice_dur),
        "thread", s.thread_name,
        "destination_thread", EXTRACT_ARG(s.arg_set_id, "destination name"),
        "destination_process", process.name,
        "flags", EXTRACT_ARG(s.arg_set_id, "flags"),
        "code", EXTRACT_ARG(s.arg_set_id, "code"),
        "data_size", EXTRACT_ARG(s.arg_set_id, "data_size")
      )
    )
    FROM SLICES_FOR_LAUNCH_AND_SLICE_NAME($launch_id, "binder transaction") s
    JOIN process ON (
      EXTRACT_ARG(s.arg_set_id, "destination process") = process.pid
    )
    WHERE s.slice_dur > 5e7
  '
);

-- Define the view 
DROP VIEW IF EXISTS startup_view;
CREATE VIEW startup_view AS
SELECT
  AndroidStartupMetric_Startup(
    'startup_id', launches.id,
    'package_name', launches.package,
    'process_name', (
      SELECT p.name
      FROM launch_processes lp
      JOIN process p USING (upid)
      WHERE lp.launch_id = launches.id
      LIMIT 1
    ),
    'process', (
      SELECT m.metadata
      FROM process_metadata m
      JOIN launch_processes p USING (upid)
      WHERE p.launch_id = launches.id
      LIMIT 1
    ),
    'activities', (
      SELECT RepeatedField(AndroidStartupMetric_Activity(
        'name', (SELECT STR_SPLIT(s.slice_name, ':', 1)),
        'method', (SELECT STR_SPLIT(s.slice_name, ':', 0)),
        'ts_method_start', s.slice_ts
      ))
      FROM thread_slices_for_all_launches s
      WHERE
        s.launch_id = launches.id AND
        (s.slice_name GLOB 'performResume:*' OR s.slice_name GLOB 'performCreate:*')
    ),
    'long_binder_transactions', BINDER_TRANSACTION_PROTO_FOR_LAUNCH(launches.id),
    'zygote_new_process', EXISTS(SELECT TRUE FROM ZYGOTE_FORK_FOR_LAUNCH(launches.id)),
    'activity_hosting_process_count', (
      SELECT COUNT(1) FROM launch_processes p
      WHERE p.launch_id = launches.id
    ),
    'event_timestamps', AndroidStartupMetric_EventTimestamps(
      'intent_received', launches.ts,
      'first_frame', launches.ts_end
    ),
    'to_first_frame', AndroidStartupMetric_ToFirstFrame(
      'dur_ns', launches.dur,
      'dur_ms', launches.dur / 1e6,
      'main_thread_by_task_state', AndroidStartupMetric_TaskStateBreakdown(
        'running_dur_ns', IFNULL(
          MAIN_THREAD_TIME_FOR_LAUNCH_AND_STATE(launches.id, 'Running'), 0
        ),
        'runnable_dur_ns', IFNULL(
          MAIN_THREAD_TIME_FOR_LAUNCH_AND_STATE(launches.id, 'R'), 0
        ),
        'uninterruptible_sleep_dur_ns', IFNULL(
          MAIN_THREAD_TIME_FOR_LAUNCH_AND_STATE(launches.id, 'D*'), 0
        ),
        'interruptible_sleep_dur_ns', IFNULL(
          MAIN_THREAD_TIME_FOR_LAUNCH_AND_STATE(launches.id, 'S'), 0
        )
      ),
      'mcycles_by_core_type', NULL_IF_EMPTY(AndroidStartupMetric_McyclesByCoreType(
        'little', MCYCLES_FOR_LAUNCH_AND_CORE_TYPE(launches.id, 'little'),
        'big', MCYCLES_FOR_LAUNCH_AND_CORE_TYPE(launches.id, 'big'),
        'bigger', MCYCLES_FOR_LAUNCH_AND_CORE_TYPE(launches.id, 'bigger'),
        'unknown', MCYCLES_FOR_LAUNCH_AND_CORE_TYPE(launches.id, 'unknown')
      )),
      'to_post_fork',
        LAUNCH_TO_MAIN_THREAD_SLICE_PROTO(launches.id, 'PostFork'),
      'to_activity_thread_main',
        LAUNCH_TO_MAIN_THREAD_SLICE_PROTO(launches.id, 'ActivityThreadMain'),
      'to_bind_application',
        LAUNCH_TO_MAIN_THREAD_SLICE_PROTO(launches.id, 'bindApplication'),
      'time_activity_manager', (
        SELECT STARTUP_SLICE_PROTO(l.ts - launches.ts)
        FROM launching_events l
        WHERE l.ts BETWEEN launches.ts AND launches.ts + launches.dur
      ),
      'time_post_fork',
        DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.id, 'PostFork'),
      'time_activity_thread_main',
        DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.id, 'ActivityThreadMain'),
      'time_bind_application',
        DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.id, 'bindApplication'),
      'time_activity_start',
        DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.id, 'activityStart'),
      'time_activity_resume',
        DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.id, 'activityResume'),
      'time_activity_restart',
        DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.id, 'activityRestart'),
      'time_choreographer',
        DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.id, 'Choreographer#doFrame*'),
      'time_inflate',
        DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.id, 'inflate'),
      'time_get_resources',
        DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.id, 'ResourcesManager#getResources'),
      'time_dex_open',
        DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.id, 'OpenDexFilesFromOat*'),
      'time_verify_class',
        DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.id, 'VerifyClass*'),
      'time_gc_total', (
        SELECT NULL_IF_EMPTY(STARTUP_SLICE_PROTO(SUM(slice_dur)))
        FROM thread_slices_for_all_launches slice
        WHERE
          launch_id = launches.id AND
          (
            slice_name GLOB "*semispace GC" OR
            slice_name GLOB "*mark sweep GC" OR
            slice_name GLOB "*concurrent copying GC"
          )
      ),
      'time_lock_contention_thread_main',
        DUR_SUM_MAIN_THREAD_SLICE_PROTO_FOR_LAUNCH(
          launches.id,
          'Lock contention on*'
        ),
      'time_monitor_contention_thread_main',
        DUR_SUM_MAIN_THREAD_SLICE_PROTO_FOR_LAUNCH(
          launches.id,
          'Lock contention on a monitor*'
        ),
      'time_before_start_process', (
        SELECT STARTUP_SLICE_PROTO(ts - launches.ts)
        FROM ZYGOTE_FORK_FOR_LAUNCH(launches.id)
      ),
      'time_jit_thread_pool_on_cpu', NULL_IF_EMPTY(STARTUP_SLICE_PROTO(
        THREAD_TIME_FOR_LAUNCH_STATE_AND_THREAD(
          launches.id,
          'Running',
          'Jit thread pool'
        )
      )),
      'time_gc_on_cpu', (
        SELECT STARTUP_SLICE_PROTO(sum_dur)
        FROM running_gc_slices_materialized
        WHERE launches.id = launch_id
      ),
      'time_during_start_process', (
        SELECT STARTUP_SLICE_PROTO(dur)
        FROM ZYGOTE_FORK_FOR_LAUNCH(launches.id)
      ),
      'jit_compiled_methods', (
        SELECT IIF(COUNT(1) = 0, NULL, COUNT(1))
        FROM SLICES_FOR_LAUNCH_AND_SLICE_NAME(launches.id, 'JIT compiling*')
        WHERE thread_name = 'Jit thread pool'
      ),
      'other_processes_spawned_count', (
        SELECT COUNT(1) FROM process
        WHERE
          (process.name IS NULL OR process.name != launches.package) AND
          process.start_ts BETWEEN launches.ts AND launches.ts + launches.dur
      )
    ),
    'hsc', NULL_IF_EMPTY(AndroidStartupMetric_HscMetrics(
      'full_startup', (
        SELECT STARTUP_SLICE_PROTO(h.ts_total)
        FROM hsc_based_startup_times h
        WHERE h.id = launches.id
      )
    )),
    'report_fully_drawn', NULL_IF_EMPTY(REPORT_FULLY_DRAWN_FOR_LAUNCH(launches.id)),
    'optimization_status', (
      SELECT RepeatedField(AndroidStartupMetric_OptimizationStatus(
        'location', SUBSTR(STR_SPLIT(slice_name, ' status=', 0), LENGTH('location=') + 1),
        'odex_status', STR_SPLIT(STR_SPLIT(slice_name, ' status=', 1), ' filter=', 0),
        'compilation_filter', STR_SPLIT(STR_SPLIT(slice_name, ' filter=', 1), ' reason=', 0),
        'compilation_reason', STR_SPLIT(slice_name, ' reason=', 1)
      ))
      FROM (
        SELECT *
        FROM SLICES_FOR_LAUNCH_AND_SLICE_NAME(
          launches.id,
          'location=* status=* filter=* reason=*'
        )
        ORDER BY slice_name
      )
    ),
    'system_state', NULL_IF_EMPTY(AndroidStartupMetric_SystemState(
      'dex2oat_running',
        IS_PROCESS_RUNNING_CONCURRENT_TO_LAUNCH(launches.id, '*dex2oat64'),
      'installd_running',
        IS_PROCESS_RUNNING_CONCURRENT_TO_LAUNCH(launches.id, '*installd') 
    ))
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
