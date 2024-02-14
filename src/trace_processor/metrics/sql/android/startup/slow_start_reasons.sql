--
-- Copyright 2022 The Android Open Source Project
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

INCLUDE PERFETTO MODULE android.startup.startups;

SELECT RUN_METRIC('android/startup/thread_state_breakdown.sql');
SELECT RUN_METRIC('android/startup/system_state.sql');
SELECT RUN_METRIC('android/startup/mcycles_per_launch.sql');

CREATE OR REPLACE PERFETTO FUNCTION is_spans_overlapping(
  ts1 LONG,
  ts_end1 LONG,
  ts2 LONG,
  ts_end2 LONG)
RETURNS BOOL AS
SELECT (IIF($ts1 < $ts2, $ts2, $ts1)
      < IIF($ts_end1 < $ts_end2, $ts_end1, $ts_end2));

CREATE OR REPLACE PERFETTO FUNCTION get_percent(num LONG, total LONG)
RETURNS STRING AS
  SELECT SUBSTRING(CAST(($num * 100 + 0.0) / $total AS STRING), 1, 5);

CREATE OR REPLACE PERFETTO FUNCTION get_ns_to_s(ns LONG)
RETURNS STRING AS
  SELECT CAST(($ns + 0.0) / 1e9 AS STRING);

CREATE OR REPLACE PERFETTO FUNCTION get_ns_to_ms(ns LONG)
RETURNS STRING AS
  SELECT SUBSTRING(CAST(($ns + 0.0) / 1e6 AS STRING), 1, 6);

CREATE OR REPLACE PERFETTO FUNCTION get_longest_chunk(start_ns LONG, dur_ns LONG, tid LONG, name STRING)
RETURNS STRING AS
  SELECT " [ longest_chunk:"
    || " start_s " || get_ns_to_s($start_ns - TRACE_START())
    || " dur_ms " || get_ns_to_ms($dur_ns)
    || " thread_id " || $tid
    || " thread_name " || $name
    || " ]";

CREATE OR REPLACE PERFETTO FUNCTION get_main_thread_time_for_launch_in_runnable_state(
  startup_id LONG, launches_dur LONG)
RETURNS STRING AS
  SELECT
    " target" || " 15%"
    || " actual "
    || get_percent(main_thread_time_for_launch_in_runnable_state($startup_id), $launches_dur)
    || "%"
    || get_longest_chunk(ts, dur, tid, name)
    || " [ extra_info: "
    || " launches_dur_ms " || get_ns_to_ms($launches_dur)
    || " runnable_dur_ms "
    || get_ns_to_ms(main_thread_time_for_launch_in_runnable_state($startup_id))
    || " R_sum_dur_ms "
    || get_ns_to_ms(IFNULL(main_thread_time_for_launch_and_state($startup_id, "R"), 0))
    || " R+(Preempted)_sum_dur_ms "
    || get_ns_to_ms(IFNULL(main_thread_time_for_launch_and_state($startup_id, "R+"), 0))
    || " ]"
  FROM launch_threads_by_thread_state l
  JOIN thread USING (utid)
  WHERE l.startup_id = $startup_id AND (state GLOB "R" OR state GLOB "R+") AND l.is_main_thread
  ORDER BY dur DESC
  LIMIT 1;

CREATE OR REPLACE PERFETTO FUNCTION get_android_sum_dur_on_main_thread_for_startup_and_slice(
  startup_id LONG, slice_name STRING, launches_dur LONG)
RETURNS STRING AS
  SELECT
    " target" || " 20%"
    || " actual "
    || get_percent(android_sum_dur_on_main_thread_for_startup_and_slice(
          $startup_id, $slice_name), $launches_dur) || "%"
    || get_longest_chunk(slice_ts, slice_dur, tid, name)
    || " [ extra_info: "
    || " launches_dur_ms " || get_ns_to_ms($launches_dur)
    || " sum_dur_ms "
    || get_ns_to_ms(android_sum_dur_on_main_thread_for_startup_and_slice($startup_id, $slice_name))
    || " ]"
    FROM android_thread_slices_for_all_startups slices
    JOIN thread USING (utid)
    WHERE startup_id = $startup_id AND slice_name GLOB $slice_name AND slices.is_main_thread
    ORDER BY slice_dur DESC
    LIMIT 1;

CREATE OR REPLACE PERFETTO FUNCTION get_android_sum_dur_for_startup_and_slice(
  startup_id LONG, slice_name STRING, target_ms LONG)
RETURNS STRING AS
  SELECT
    " target " || $target_ms || "ms"
    || " actual "
    || get_ns_to_ms(android_sum_dur_for_startup_and_slice($startup_id, $slice_name)) || "ms"
    || get_longest_chunk(slice_ts, slice_dur, tid, name)
    FROM android_thread_slices_for_all_startups
    JOIN thread USING (utid)
    WHERE startup_id = $startup_id AND slice_name GLOB $slice_name
    ORDER BY slice_dur DESC
    LIMIT 1;

CREATE OR REPLACE PERFETTO FUNCTION get_potential_cpu_contention_with_another_process(startup_id LONG)
RETURNS STRING AS
  SELECT
    " target" || " 100ms"
    || " actual "
    || get_ns_to_ms(main_thread_time_for_launch_in_runnable_state($startup_id)) || "ms"
    || " most_active_process_for_launch " || most_active_process_for_launch($startup_id)
    || get_longest_chunk(ts, dur, tid, name)
    || " [ extra_info: "
    || " runnable_dur_ms "
    || get_ns_to_ms(main_thread_time_for_launch_in_runnable_state($startup_id))
    || " R_sum_dur_ms "
    || get_ns_to_ms(IFNULL(main_thread_time_for_launch_and_state($startup_id, "R"), 0))
    || " R+(Preempted)_sum_dur "
    || IFNULL(main_thread_time_for_launch_and_state($startup_id, "R+"), 0)
    || " ]"
  FROM launch_threads_by_thread_state l
  JOIN thread USING (utid)
  WHERE l.startup_id = $startup_id AND (state GLOB "R" OR state GLOB "R+") AND l.is_main_thread
  ORDER BY dur DESC
  LIMIT 1;

CREATE OR REPLACE PERFETTO FUNCTION get_jit_activity(startup_id LONG)
RETURNS STRING AS
  SELECT
    " target" || " 100ms"
    || " actual "
    || get_ns_to_ms(thread_time_for_launch_state_and_thread(
      $startup_id, 'Running', 'Jit thread pool'))
    || "ms"
    || get_longest_chunk(ts, dur, tid, name)
  FROM launch_threads_by_thread_state l
  JOIN thread USING (utid)
  WHERE l.startup_id = $startup_id AND state GLOB 'Running' AND thread_name = 'Jit thread pool'
  ORDER BY dur DESC
  LIMIT 1;

CREATE OR REPLACE PERFETTO FUNCTION get_main_thread_binder_transactions_blocked(
  startup_id LONG, threshold DOUBLE)
RETURNS STRING AS
  SELECT
    " per_instance_target" || " 20ms"
    || " per_instance_actual " || get_ns_to_ms(request.slice_dur) || "ms"
    || get_longest_chunk(request.slice_ts, request.slice_dur, tid, request.thread_name)
    || " [ extra_info: "
    || " reply.dur_ms " || get_ns_to_ms(reply.dur)
    || " ]"
  FROM (
    SELECT slice_id as id, slice_dur, thread_name, process.name as process,
      s.arg_set_id, is_main_thread,
      slice_ts, s.utid
    FROM android_thread_slices_for_all_startups s
    JOIN process ON (
      EXTRACT_ARG(s.arg_set_id, "destination process") = process.pid
    )
    WHERE startup_id = $startup_id AND slice_name GLOB "binder transaction"
      AND slice_dur > $threshold
  ) request
  JOIN following_flow(request.id) arrow
  JOIN slice reply ON reply.id = arrow.slice_in
  JOIN thread USING (utid)
  WHERE reply.dur > $threshold AND request.is_main_thread
  ORDER BY request.slice_dur DESC
  LIMIT 1;

CREATE OR REPLACE PERFETTO FUNCTION get_missing_baseline_profile_for_launch(
  startup_id LONG, pkg_name STRING)
RETURNS STRING AS
  SELECT
    " target " || "FALSE"
    || " actual " || "TRUE"
    || get_longest_chunk(slice_ts, slice_dur, -1, thread_name)
    || " [ extra_info: "
    || " slice_name " || slice_name
    || " ]"
    FROM (
      SELECT *
      FROM ANDROID_SLICES_FOR_STARTUP_AND_SLICE_NAME(
        $startup_id,
        "location=* status=* filter=* reason=*"
      )
      ORDER BY slice_name
    )
    WHERE
      -- when location is the package odex file and the reason is "install" or "install-dm",
      -- if the compilation filter is not "speed-profile", baseline/cloud profile is missing.
      SUBSTR(STR_SPLIT(slice_name, " status=", 0), LENGTH("location=") + 1)
        GLOB ("*" || $pkg_name || "*odex")
      AND (STR_SPLIT(slice_name, " reason=", 1) = "install"
        OR STR_SPLIT(slice_name, " reason=", 1) = "install-dm")
    ORDER BY slice_dur DESC
    LIMIT 1;


CREATE OR REPLACE PERFETTO FUNCTION get_slow_start_reason_detailed(startup_id LONG)
RETURNS PROTO AS
      SELECT RepeatedField(AndroidStartupMetric_SlowStartReasonDetailed(
        'reason', slow_cause,
        'details', details))
      FROM (
        SELECT 'No baseline or cloud profiles' AS slow_cause,
          get_missing_baseline_profile_for_launch(launch.startup_id, launch.package) as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND missing_baseline_profile_for_launch(launch.startup_id, launch.package)

        UNION ALL
        SELECT 'Optimized artifacts missing, run from apk' as slow_cause, NULL as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND  run_from_apk_for_launch(launch.startup_id)

        UNION ALL
        SELECT 'Unlock running during launch' as slow_cause, NULL as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
         AND is_unlock_running_during_launch(launch.startup_id)

        UNION ALL
        SELECT 'App in debuggable mode' as slow_cause, NULL as details
       	FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND is_process_debuggable(launch.package)

        UNION ALL
        SELECT 'GC Activity' as slow_cause, NULL as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND total_gc_time_by_launch(launch.startup_id) > 0

        UNION ALL
        SELECT 'dex2oat running during launch' AS slow_cause, NULL as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id AND
          dur_of_process_running_concurrent_to_launch(launch.startup_id, '*dex2oat64') > 0

        UNION ALL
        SELECT 'installd running during launch' AS slow_cause, NULL as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id AND
          dur_of_process_running_concurrent_to_launch(launch.startup_id, '*installd') > 0

        UNION ALL
        SELECT 'Main Thread - Time spent in Runnable state' as slow_cause,
          get_main_thread_time_for_launch_in_runnable_state(
            launch.startup_id, launch.dur) as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND main_thread_time_for_launch_in_runnable_state(launch.startup_id) > launch.dur * 0.15

        UNION ALL
        SELECT 'Main Thread - Time spent in interruptible sleep state'
          AS slow_cause, NULL as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND main_thread_time_for_launch_and_state(launch.startup_id, 'S') > 2900e6

        UNION ALL
        SELECT 'Main Thread - Time spent in Blocking I/O' as slow_cause, NULL as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND main_thread_time_for_launch_state_and_io_wait(launch.startup_id, 'D*', TRUE) > 450e6

        UNION ALL
        SELECT 'Main Thread - Time spent in OpenDexFilesFromOat*' as slow_cause,
          get_android_sum_dur_on_main_thread_for_startup_and_slice(
            launch.startup_id, 'OpenDexFilesFromOat*', launch.dur) as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id AND
          android_sum_dur_on_main_thread_for_startup_and_slice(
          launch.startup_id, 'OpenDexFilesFromOat*') > launch.dur * 0.2

        UNION ALL
        SELECT 'Time spent in bindApplication'
          AS slow_cause, NULL as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND android_sum_dur_for_startup_and_slice(launch.startup_id, 'bindApplication') > 1250e6

        UNION ALL
        SELECT 'Time spent in view inflation' as slow_cause, NULL as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND android_sum_dur_for_startup_and_slice(launch.startup_id, 'inflate') > 450e6

        UNION ALL
        SELECT 'Time spent in ResourcesManager#getResources' as slow_cause,
          get_android_sum_dur_for_startup_and_slice(
            launch.startup_id, 'ResourcesManager#getResources', 130) as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND android_sum_dur_for_startup_and_slice(
          launch.startup_id, 'ResourcesManager#getResources') > 130e6

        UNION ALL
        SELECT 'Time spent verifying classes'
          AS slow_cause, NULL as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id AND
          android_sum_dur_for_startup_and_slice(launch.startup_id, 'VerifyClass*')
            > launch.dur * 0.15

        UNION ALL
        SELECT 'Potential CPU contention with another process' AS slow_cause,
          get_potential_cpu_contention_with_another_process(launch.startup_id) as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id AND
          main_thread_time_for_launch_in_runnable_state(launch.startup_id) > 100e6 AND
          most_active_process_for_launch(launch.startup_id) IS NOT NULL

        UNION ALL
        SELECT 'JIT Activity' as slow_cause,
          get_jit_activity(launch.startup_id) as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
        AND thread_time_for_launch_state_and_thread(
          launch.startup_id,
          'Running',
          'Jit thread pool'
        ) > 100e6

        UNION ALL
        SELECT 'Main Thread - Lock contention'
          AS slow_cause, NULL as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND android_sum_dur_on_main_thread_for_startup_and_slice(
          launch.startup_id,
          'Lock contention on*'
        ) > launch.dur * 0.2

        UNION ALL
        SELECT 'Main Thread - Monitor contention'
          AS slow_cause, NULL as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND android_sum_dur_on_main_thread_for_startup_and_slice(
          launch.startup_id,
          'Lock contention on a monitor*'
        ) > launch.dur * 0.15

        UNION ALL
        SELECT 'JIT compiled methods' as slow_cause, NULL as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND (
          SELECT COUNT(1)
          FROM ANDROID_SLICES_FOR_STARTUP_AND_SLICE_NAME(launch.startup_id, 'JIT compiling*')
          WHERE thread_name = 'Jit thread pool'
        ) > 65

        UNION ALL
        SELECT 'Broadcast dispatched count' as slow_cause, NULL as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND count_slices_concurrent_to_launch(
          launch.startup_id,
          'Broadcast dispatched*'
        ) > 15

        UNION ALL
        SELECT 'Broadcast received count' as slow_cause, NULL as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND count_slices_concurrent_to_launch(
          launch.startup_id,
          'broadcastReceiveReg*'
        ) > 50

        UNION ALL
        SELECT 'Startup running concurrent to launch' as slow_cause, NULL as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND EXISTS(
          SELECT package
          FROM android_startups l
          WHERE l.startup_id != launch.startup_id
            AND is_spans_overlapping(l.ts, l.ts_end, launch.ts, launch.ts_end)
        )

        UNION ALL
        SELECT 'Main Thread - Binder transactions blocked' as slow_cause,
          get_main_thread_binder_transactions_blocked(launch.startup_id, 2e7) as details
        FROM android_startups launch
        WHERE launch.startup_id = $startup_id
          AND (
          SELECT COUNT(1)
          FROM BINDER_TRANSACTION_REPLY_SLICES_FOR_LAUNCH(launch.startup_id, 2e7)
        ) > 0
      );
