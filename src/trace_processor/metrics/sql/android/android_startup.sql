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
SELECT IMPORT('android.startup.startups');
SELECT RUN_METRIC('android/process_metadata.sql');

-- Define the helper functions which will be used throught the remainder
-- of the metric.
SELECT RUN_METRIC('android/startup/slice_functions.sql');
SELECT IMPORT('common.timestamps');

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

-- Returns the slices for forked processes. Never present in hot starts.
-- Prefer this over process start_ts, since the process might have
-- been preforked.
SELECT CREATE_VIEW_FUNCTION(
  'ZYGOTE_FORK_FOR_LAUNCH(startup_id INT)',
  'ts INT, dur INT',
  '
    SELECT slice.ts, slice.dur
    FROM android_startups l
    JOIN slice ON (
      l.ts < slice.ts AND
      slice.ts + slice.dur < l.ts_end AND
      STR_SPLIT(slice.name, ": ", 1) = l.package
    )
    WHERE l.startup_id = $startup_id AND slice.name GLOB "Start proc: *"
  '
);

-- Returns the fully drawn slice proto given a launch id.
SELECT CREATE_FUNCTION(
  'REPORT_FULLY_DRAWN_FOR_LAUNCH(startup_id INT)',
  'PROTO',
  '
    SELECT
      STARTUP_SLICE_PROTO(report_fully_drawn_ts - launch_ts)
    FROM (
      SELECT
        launches.ts AS launch_ts,
        min(slice.ts) AS report_fully_drawn_ts
      FROM android_startups launches
      JOIN android_startup_processes ON (launches.startup_id = android_startup_processes.startup_id)
      JOIN thread USING (upid)
      JOIN thread_track USING (utid)
      JOIN slice ON (slice.track_id = thread_track.id)
      WHERE
        slice.name GLOB "reportFullyDrawn*" AND
        slice.ts >= launches.ts AND
        launches.startup_id = $startup_id
    )
  '
);

-- Define the view
DROP VIEW IF EXISTS startup_view;
CREATE VIEW startup_view AS
SELECT
  AndroidStartupMetric_Startup(
    'startup_id',launches.startup_id,
    'startup_type', (
      SELECT lp.startup_type
      FROM android_startup_processes lp
      WHERE lp.startup_id =launches.startup_id
      LIMIT 1
    ),
    'package_name', launches.package,
    'process_name', (
      SELECT p.name
      FROM android_startup_processes lp
      JOIN process p USING (upid)
      WHERE lp.startup_id =launches.startup_id
      LIMIT 1
    ),
    'process', (
      SELECT m.metadata
      FROM process_metadata m
      JOIN android_startup_processes p USING (upid)
      WHERE p.startup_id =launches.startup_id
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
        s.startup_id =launches.startup_id
        AND (s.slice_name GLOB 'performResume:*' OR s.slice_name GLOB 'performCreate:*')
    ),
    'long_binder_transactions', (
      SELECT RepeatedField(
        AndroidStartupMetric_BinderTransaction(
          "duration", STARTUP_SLICE_PROTO(s.slice_dur),
          "thread", s.thread_name,
          "destination_thread", EXTRACT_ARG(s.arg_set_id, "destination name"),
          "destination_process", s.process,
          "flags", EXTRACT_ARG(s.arg_set_id, "flags"),
          "code", EXTRACT_ARG(s.arg_set_id, "code"),
          "data_size", EXTRACT_ARG(s.arg_set_id, "data_size")
        )
      )
      FROM ANDROID_BINDER_TRANSACTION_SLICES_FOR_STARTUP(launches.startup_id, 5e7) s
    ),
    'zygote_new_process', EXISTS(SELECT TRUE FROM ZYGOTE_FORK_FOR_LAUNCH(launches.startup_id)),
    'activity_hosting_process_count', (
      SELECT COUNT(1) FROM android_startup_processes p
      WHERE p.startup_id =launches.startup_id
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
          MAIN_THREAD_TIME_FOR_LAUNCH_AND_STATE(launches.startup_id, 'Running'), 0
        ),
        'runnable_dur_ns', IFNULL(
          MAIN_THREAD_TIME_FOR_LAUNCH_IN_RUNNABLE_STATE(launches.startup_id), 0
        ),
        'uninterruptible_sleep_dur_ns', IFNULL(
          MAIN_THREAD_TIME_FOR_LAUNCH_AND_STATE(launches.startup_id, 'D*'), 0
        ),
        'interruptible_sleep_dur_ns', IFNULL(
          MAIN_THREAD_TIME_FOR_LAUNCH_AND_STATE(launches.startup_id, 'S'), 0
        ),
        'uninterruptible_io_sleep_dur_ns', IFNULL(
          MAIN_THREAD_TIME_FOR_LAUNCH_STATE_AND_IO_WAIT(launches.startup_id, 'D*', TRUE), 0
        ),
        'uninterruptible_non_io_sleep_dur_ns', IFNULL(
          MAIN_THREAD_TIME_FOR_LAUNCH_STATE_AND_IO_WAIT(launches.startup_id, 'D*', FALSE), 0
        )

      ),
      'mcycles_by_core_type', NULL_IF_EMPTY(AndroidStartupMetric_McyclesByCoreType(
        'little', MCYCLES_FOR_LAUNCH_AND_CORE_TYPE(launches.startup_id, 'little'),
        'big', MCYCLES_FOR_LAUNCH_AND_CORE_TYPE(launches.startup_id, 'big'),
        'bigger', MCYCLES_FOR_LAUNCH_AND_CORE_TYPE(launches.startup_id, 'bigger'),
        'unknown', MCYCLES_FOR_LAUNCH_AND_CORE_TYPE(launches.startup_id, 'unknown')
      )),
      'to_post_fork',
      LAUNCH_TO_MAIN_THREAD_SLICE_PROTO(launches.startup_id, 'PostFork'),
      'to_activity_thread_main',
      LAUNCH_TO_MAIN_THREAD_SLICE_PROTO(launches.startup_id, 'ActivityThreadMain'),
      'to_bind_application',
      LAUNCH_TO_MAIN_THREAD_SLICE_PROTO(launches.startup_id, 'bindApplication'),
      'time_activity_manager', (
        SELECT STARTUP_SLICE_PROTO(l.ts - launches.ts)
        FROM internal_startup_events l
        WHERE l.ts BETWEEN launches.ts AND launches.ts + launches.dur
      ),
      'time_post_fork',
      DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.startup_id, 'PostFork'),
      'time_activity_thread_main',
      DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.startup_id, 'ActivityThreadMain'),
      'time_bind_application',
      DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.startup_id, 'bindApplication'),
      'time_activity_start',
      DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.startup_id, 'activityStart'),
      'time_activity_resume',
      DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.startup_id, 'activityResume'),
      'time_activity_restart',
      DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.startup_id, 'activityRestart'),
      'time_choreographer',
      DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.startup_id, 'Choreographer#doFrame*'),
      'time_inflate',
      DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.startup_id, 'inflate'),
      'time_get_resources',
      DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.startup_id, 'ResourcesManager#getResources'),
      'time_dex_open',
      DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.startup_id, 'OpenDexFilesFromOat*'),
      'time_verify_class',
      DUR_SUM_SLICE_PROTO_FOR_LAUNCH(launches.startup_id, 'VerifyClass*'),
      'time_gc_total', (
        SELECT NULL_IF_EMPTY(STARTUP_SLICE_PROTO(TOTAL_GC_TIME_BY_LAUNCH(launches.startup_id)))
      ),
      'time_lock_contention_thread_main',
      DUR_SUM_MAIN_THREAD_SLICE_PROTO_FOR_LAUNCH(
       launches.startup_id,
        'Lock contention on*'
      ),
      'time_monitor_contention_thread_main',
      DUR_SUM_MAIN_THREAD_SLICE_PROTO_FOR_LAUNCH(
       launches.startup_id,
        'Lock contention on a monitor*'
      ),
      'time_before_start_process', (
        SELECT STARTUP_SLICE_PROTO(ts - launches.ts)
        FROM ZYGOTE_FORK_FOR_LAUNCH(launches.startup_id)
      ),
      'time_jit_thread_pool_on_cpu', NULL_IF_EMPTY(STARTUP_SLICE_PROTO(
        THREAD_TIME_FOR_LAUNCH_STATE_AND_THREAD(
         launches.startup_id,
          'Running',
          'Jit thread pool'
        )
      )),
      'time_gc_on_cpu', (
        SELECT STARTUP_SLICE_PROTO(sum_dur)
        FROM running_gc_slices_materialized
        WHERE launches.startup_id = startup_id
      ),
      'time_during_start_process', (
        SELECT STARTUP_SLICE_PROTO(dur)
        FROM ZYGOTE_FORK_FOR_LAUNCH(launches.startup_id)
      ),
      'jit_compiled_methods', (
        SELECT IIF(COUNT(1) = 0, NULL, COUNT(1))
        FROM ANDROID_SLICES_FOR_STARTUP_AND_SLICE_NAME(launches.startup_id, 'JIT compiling*')
        WHERE thread_name = 'Jit thread pool'
      ),
      'other_processes_spawned_count', (
        SELECT COUNT(1)
        FROM process
        WHERE
          process.start_ts BETWEEN launches.ts AND launches.ts + launches.dur
          AND process.upid NOT IN (
            SELECT upid FROM android_startup_processes
            WHERE android_startup_processes.startup_id =launches.startup_id
          )
      )
    ),
    'hsc', NULL_IF_EMPTY(AndroidStartupMetric_HscMetrics(
      'full_startup', (
        SELECT STARTUP_SLICE_PROTO(h.ts_total)
        FROM hsc_based_startup_times h
        WHERE h.id =launches.startup_id
      )
    )),
    'report_fully_drawn', NULL_IF_EMPTY(REPORT_FULLY_DRAWN_FOR_LAUNCH(launches.startup_id)),
    'optimization_status', (
      SELECT RepeatedField(AndroidStartupMetric_OptimizationStatus(
        'location', SUBSTR(STR_SPLIT(slice_name, ' status=', 0), LENGTH('location=') + 1),
        'odex_status', STR_SPLIT(STR_SPLIT(slice_name, ' status=', 1), ' filter=', 0),
        'compilation_filter', STR_SPLIT(STR_SPLIT(slice_name, ' filter=', 1), ' reason=', 0),
        'compilation_reason', STR_SPLIT(slice_name, ' reason=', 1)
        ))
      FROM (
        SELECT *
        FROM ANDROID_SLICES_FOR_STARTUP_AND_SLICE_NAME(
         launches.startup_id,
          'location=* status=* filter=* reason=*'
        )
        ORDER BY slice_name
      )
    ),
    'startup_concurrent_to_launch', (
      SELECT RepeatedField(package)
      FROM android_startups l
      WHERE l.startup_id != launches.startup_id
        AND IS_SPANS_OVERLAPPING(l.ts, l.ts_end, launches.ts, launches.ts_end)
    ),
    'system_state', AndroidStartupMetric_SystemState(
      'dex2oat_running',
      DUR_OF_PROCESS_RUNNING_CONCURRENT_TO_LAUNCH(launches.startup_id, '*dex2oat64') > 0,
      'installd_running',
      DUR_OF_PROCESS_RUNNING_CONCURRENT_TO_LAUNCH(launches.startup_id, '*installd') > 0,
      'broadcast_dispatched_count',
      COUNT_SLICES_CONCURRENT_TO_LAUNCH(launches.startup_id, 'Broadcast dispatched*'),
      'broadcast_received_count',
      COUNT_SLICES_CONCURRENT_TO_LAUNCH(launches.startup_id, 'broadcastReceiveReg*'),
      'most_active_non_launch_processes',
      N_MOST_ACTIVE_PROCESS_NAMES_FOR_LAUNCH(launches.startup_id),
      'installd_dur_ns',
      DUR_OF_PROCESS_RUNNING_CONCURRENT_TO_LAUNCH(launches.startup_id, '*installd'),
      'dex2oat_dur_ns',
      DUR_OF_PROCESS_RUNNING_CONCURRENT_TO_LAUNCH(launches.startup_id, '*dex2oat64')
    ),
    'slow_start_reason', (SELECT RepeatedField(slow_cause)
      FROM (
        SELECT 'dex2oat running during launch' AS slow_cause
        WHERE
          DUR_OF_PROCESS_RUNNING_CONCURRENT_TO_LAUNCH(launches.startup_id, '*dex2oat64') > 20e6

        UNION ALL
        SELECT 'installd running during launch' AS slow_cause
        WHERE
          DUR_OF_PROCESS_RUNNING_CONCURRENT_TO_LAUNCH(launches.startup_id, '*installd') > 150e6

        UNION ALL
        SELECT 'Main Thread - Time spent in Running state'
          AS slow_cause
        WHERE MAIN_THREAD_TIME_FOR_LAUNCH_AND_STATE(launches.startup_id, 'Running') > 150e6

        UNION ALL
        SELECT 'Main Thread - Time spent in Runnable state'
          AS slow_cause
        WHERE MAIN_THREAD_TIME_FOR_LAUNCH_IN_RUNNABLE_STATE(launches.startup_id) > 100e6

        UNION ALL
        SELECT 'Main Thread - Time spent in interruptible sleep state'
          AS slow_cause
        WHERE MAIN_THREAD_TIME_FOR_LAUNCH_AND_STATE(launches.startup_id, 'S') > 250e6

        UNION ALL
        SELECT 'Main Thread - Time spent in Blocking I/O'
        WHERE MAIN_THREAD_TIME_FOR_LAUNCH_STATE_AND_IO_WAIT(launches.startup_id, 'D*', TRUE) > 300e6

        UNION ALL
        SELECT 'Time spent in OpenDexFilesFromOat*'
          AS slow_cause
        WHERE ANDROID_SUM_DUR_FOR_STARTUP_AND_SLICE(launches.startup_id, 'OpenDexFilesFromOat*') > 1e6

        UNION ALL
        SELECT 'Time spent in bindApplication'
          AS slow_cause
        WHERE ANDROID_SUM_DUR_FOR_STARTUP_AND_SLICE(launches.startup_id, 'bindApplication') > 10e6

        UNION ALL
        SELECT 'Time spent in view inflation'
          AS slow_cause
        WHERE ANDROID_SUM_DUR_FOR_STARTUP_AND_SLICE(launches.startup_id, 'inflate') > 600e6

        UNION ALL
        SELECT 'Time spent in ResourcesManager#getResources'
          AS slow_cause
        WHERE ANDROID_SUM_DUR_FOR_STARTUP_AND_SLICE(
          launches.startup_id, 'ResourcesManager#getResources') > 10e6

        UNION ALL
        SELECT 'Time spent verifying classes'
          AS slow_cause
        WHERE ANDROID_SUM_DUR_FOR_STARTUP_AND_SLICE(launches.startup_id, 'VerifyClass*') > 10e6

        UNION ALL
        SELECT 'Potential CPU contention with '
          || MOST_ACTIVE_PROCESS_FOR_LAUNCH(launches.startup_id)
          AS slow_cause
        WHERE MAIN_THREAD_TIME_FOR_LAUNCH_IN_RUNNABLE_STATE(launches.startup_id) > 100e6
          AND MOST_ACTIVE_PROCESS_FOR_LAUNCH(launches.startup_id) IS NOT NULL

        UNION ALL
        SELECT 'JIT Activity'
          AS slow_cause
        WHERE THREAD_TIME_FOR_LAUNCH_STATE_AND_THREAD(
          launches.startup_id,
          'Running',
          'Jit thread pool'
        ) > 120e6

        UNION ALL
        SELECT 'Main Thread - Lock contention'
          AS slow_cause
        WHERE ANDROID_SUM_DUR_ON_MAIN_THREAD_FOR_STARTUP_AND_SLICE(
          launches.startup_id,
          'Lock contention on*'
        ) > 25e6

        UNION ALL
        SELECT 'Main Thread - Monitor contention'
          AS slow_cause
        WHERE ANDROID_SUM_DUR_ON_MAIN_THREAD_FOR_STARTUP_AND_SLICE(
          launches.startup_id,
          'Lock contention on a monitor*'
        ) > 40e6

        UNION ALL
        SELECT 'GC Activity'
        WHERE TOTAL_GC_TIME_BY_LAUNCH(launches.startup_id) > 0

        UNION ALL
        SELECT 'JIT compiled methods'
        WHERE (
          SELECT COUNT(1)
          FROM ANDROID_SLICES_FOR_STARTUP_AND_SLICE_NAME(launches.startup_id, 'JIT compiling*')
          WHERE thread_name = 'Jit thread pool'
        ) > 40

        UNION ALL
        SELECT 'Broadcast dispatched count'
        WHERE COUNT_SLICES_CONCURRENT_TO_LAUNCH(
          launches.startup_id,
          'Broadcast dispatched*'
        ) > 10

        UNION ALL
        SELECT 'Broadcast received count'
        WHERE COUNT_SLICES_CONCURRENT_TO_LAUNCH(
          launches.startup_id,
          'broadcastReceiveReg*'
        ) > 10

        UNION ALL
        SELECT 'No baseline or cloud profiles'
        WHERE MISSING_BASELINE_PROFILE_FOR_LAUNCH(launches.startup_id, launches.package)

        UNION ALL
        SELECT 'Optimized artifacts missing, run from apk'
        WHERE  RUN_FROM_APK_FOR_LAUNCH(launches.startup_id)

        UNION ALL
        SELECT 'Startup running concurrent to launch'
        WHERE EXISTS(
          SELECT package
          FROM android_startups l
          WHERE l.startup_id != launches.startup_id
            AND IS_SPANS_OVERLAPPING(l.ts, l.ts_end, launches.ts, launches.ts_end)
        )

        UNION ALL
        SELECT 'Main Thread - Binder transactions blocked'
        WHERE (
          SELECT COUNT(1)
          FROM BINDER_TRANSACTION_REPLY_SLICES_FOR_LAUNCH(launches.startup_id, 2e7)
        ) > 0

        UNION ALL
        SELECT 'Unlock running during launch'
        WHERE IS_UNLOCK_RUNNING_DURING_LAUNCH(launches.startup_id)

      )
    )
  ) AS startup
FROM android_startups launches;

DROP VIEW IF EXISTS android_startup_event;
CREATE VIEW android_startup_event AS
SELECT
  'slice' AS track_type,
  'Android App Startups' AS track_name,
  l.ts AS ts,
  l.dur AS dur,
  l.package AS slice_name
FROM android_startups l;

DROP VIEW IF EXISTS android_startup_output;
CREATE VIEW android_startup_output AS
SELECT
  AndroidStartupMetric(
    'startup', (
      SELECT RepeatedField(startup) FROM startup_view
    )
  );
