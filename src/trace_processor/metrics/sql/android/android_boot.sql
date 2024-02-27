--
-- Copyright 2023 The Android Open Source Project
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

INCLUDE PERFETTO MODULE android.process_metadata;
INCLUDE PERFETTO MODULE android.app_process_starts;
INCLUDE PERFETTO MODULE android.garbage_collection;

CREATE OR REPLACE PERFETTO FUNCTION get_durations(process_name STRING)
RETURNS TABLE(uint_sleep_dur LONG, total_dur LONG) AS
SELECT
    SUM(CASE WHEN thread_state.state="D" then thread_state.dur ELSE 0 END) AS uint_sleep_dur,
    SUM(thread_state.dur) as total_dur
FROM android_process_metadata
INNER JOIN thread ON thread.upid=android_process_metadata.upid
INNER JOIN thread_state ON thread.utid=thread_state.utid WHERE android_process_metadata.process_name=$process_name;

DROP VIEW IF EXISTS android_boot_output;
CREATE PERFETTO VIEW android_boot_output AS
SELECT AndroidBootMetric(
    'system_server_durations', (
        SELECT NULL_IF_EMPTY(ProcessStateDurations(
            'total_dur', total_dur,
            'uninterruptible_sleep_dur', uint_sleep_dur))
        FROM get_durations('system_server')),
    'systemui_durations', (
        SELECT NULL_IF_EMPTY(ProcessStateDurations(
            'total_dur', total_dur,
            'uninterruptible_sleep_dur', uint_sleep_dur))
        FROM get_durations('com.android.systemui')),
    'launcher_durations', (
        SELECT NULL_IF_EMPTY(ProcessStateDurations(
            'total_dur', total_dur,
            'uninterruptible_sleep_dur', uint_sleep_dur))
        FROM get_durations('com.google.android.apps.nexuslauncher')),
    'gms_durations', (
        SELECT NULL_IF_EMPTY(ProcessStateDurations(
            'total_dur', total_dur,
            'uninterruptible_sleep_dur', uint_sleep_dur))
        FROM get_durations('com.google.android.gms.persistent')),
    'launcher_breakdown', (
        SELECT NULL_IF_EMPTY(AndroidBootMetric_LauncherBreakdown(
            'cold_start_dur', dur))
        FROM slice where name="LauncherColdStartup"),
    'full_trace_process_start_aggregation', (
        SELECT NULL_IF_EMPTY(AndroidBootMetric_ProcessStartAggregation(
            'total_start_sum', (SELECT SUM(total_dur) FROM android_app_process_starts),
            'num_of_processes', (SELECT COUNT(*) FROM android_app_process_starts),
            'average_start_time', (SELECT AVG(total_dur) FROM android_app_process_starts)))
            FROM android_app_process_starts),
    'post_boot_process_start_aggregation', (
        SELECT NULL_IF_EMPTY(AndroidBootMetric_ProcessStartAggregation(
            'total_start_sum', (SELECT SUM(total_dur) FROM android_app_process_starts
              WHERE proc_start_ts > (SELECT COALESCE(MIN(ts), 0)
                FROM thread_slice WHERE name GLOB "*android.intent.action.USER_UNLOCKED*"
                ORDER BY ts ASC LIMIT 1 )
            ),
            'num_of_processes', (SELECT COUNT(*) FROM android_app_process_starts
              WHERE proc_start_ts > (SELECT COALESCE(MIN(ts), 0) FROM thread_slice
                WHERE name GLOB "*android.intent.action.USER_UNLOCKED*" ORDER BY ts
                ASC LIMIT 1 )
            ),
            'average_start_time', (SELECT AVG(total_dur) FROM android_app_process_starts
              WHERE proc_start_ts > (SELECT COALESCE(MIN(ts), 0) FROM thread_slice
                WHERE name GLOB "*android.intent.action.USER_UNLOCKED*" ORDER BY ts
                ASC LIMIT 1 )
            )
        ))
    ),
    'full_trace_gc_aggregation', (
        SELECT NULL_IF_EMPTY(AndroidBootMetric_GarbageCollectionAggregation(
            'total_gc_count', (SELECT COUNT(*) FROM android_garbage_collection_events
            ),
            'num_of_processes_with_gc', (SELECT COUNT(process_name) FROM android_garbage_collection_events
            ),
            'num_of_threads_with_gc', (SELECT SUM(cnt) FROM (SELECT COUNT(*) AS cnt
              FROM android_garbage_collection_events
              GROUP by thread_name, process_name)
            ),
            'avg_gc_duration', (SELECT AVG(gc_dur) FROM android_garbage_collection_events),
            'avg_running_gc_duration', (SELECT AVG(gc_running_dur) FROM android_garbage_collection_events),
            'full_gc_count', (SELECT COUNT(*) FROM android_garbage_collection_events
              WHERE gc_type = "full"
            ),
            'collector_transition_gc_count', (SELECT COUNT(*) FROM android_garbage_collection_events
              WHERE gc_type = "collector_transition"
            ),
            'young_gc_count', (SELECT COUNT(*) FROM android_garbage_collection_events
              WHERE gc_type = "young"
            ),
            'native_alloc_gc_count', (SELECT COUNT(*) FROM android_garbage_collection_events
              WHERE gc_type = "native_alloc"
            ),
            'explicit_gc_count', (SELECT COUNT(*) FROM android_garbage_collection_events
              WHERE gc_type = "explicit_gc"
            ),
            'alloc_gc_count', (SELECT COUNT(*) FROM android_garbage_collection_events
              WHERE gc_type = "alloc_gc"
            ),
            'mb_per_ms_of_gc', (SELECT SUM(reclaimed_mb)/SUM(gc_running_dur/1e6) AS mb_per_ms_dur
              FROM android_garbage_collection_events
            )
        ))
    ),
    'post_boot_gc_aggregation', (
        SELECT NULL_IF_EMPTY(AndroidBootMetric_GarbageCollectionAggregation(
            'total_gc_count', (SELECT COUNT(*) FROM android_garbage_collection_events
              WHERE gc_ts > (SELECT COALESCE(MIN(ts), 0)
                FROM thread_slice WHERE name GLOB "*android.intent.action.USER_UNLOCKED*"
                ORDER BY ts ASC LIMIT 1 )
            ),
            'num_of_processes_with_gc', (SELECT COUNT(process_name) FROM android_garbage_collection_events
              WHERE gc_ts > (SELECT COALESCE(MIN(ts), 0)
                FROM thread_slice WHERE name GLOB "*android.intent.action.USER_UNLOCKED*"
                ORDER BY ts ASC LIMIT 1 )
            ),
            'num_of_threads_with_gc', (SELECT SUM(cnt) FROM (SELECT COUNT(*) AS cnt
              FROM android_garbage_collection_events
              WHERE gc_ts > (SELECT COALESCE(MIN(ts), 0) FROM thread_slice
                WHERE name GLOB "*android.intent.action.USER_UNLOCKED*" ORDER BY ts
                ASC LIMIT 1 )
              GROUP by thread_name, process_name)
            ),
            'avg_gc_duration', (SELECT AVG(gc_dur) FROM android_garbage_collection_events
              WHERE gc_ts > (SELECT COALESCE(MIN(ts), 0) FROM thread_slice
                WHERE name GLOB "*android.intent.action.USER_UNLOCKED*" ORDER BY ts
                ASC LIMIT 1 )
            ),
            'avg_running_gc_duration', (SELECT AVG(gc_running_dur) FROM android_garbage_collection_events
              WHERE gc_ts > (SELECT COALESCE(MIN(ts), 0) FROM thread_slice
                WHERE name GLOB "*android.intent.action.USER_UNLOCKED*" ORDER BY ts
                ASC LIMIT 1 )
            ),
            'full_gc_count', (SELECT COUNT(*) FROM android_garbage_collection_events
              WHERE gc_type = "full" AND gc_ts > (SELECT COALESCE(MIN(ts), 0)
                FROM thread_slice WHERE name GLOB "*android.intent.action.USER_UNLOCKED*"
                ORDER BY ts ASC LIMIT 1 )
            ),
            'collector_transition_gc_count', (SELECT COUNT(*) FROM android_garbage_collection_events
              WHERE gc_type = "collector_transition" AND gc_ts > (SELECT COALESCE(MIN(ts), 0)
                FROM thread_slice WHERE name GLOB "*android.intent.action.USER_UNLOCKED*"
                ORDER BY ts ASC LIMIT 1 )
            ),
            'young_gc_count', (SELECT COUNT(*) FROM android_garbage_collection_events
              WHERE gc_type = "young" AND gc_ts > (SELECT COALESCE(MIN(ts), 0)
                FROM thread_slice WHERE name GLOB "*android.intent.action.USER_UNLOCKED*"
                ORDER BY ts ASC LIMIT 1 )
            ),
            'native_alloc_gc_count', (SELECT COUNT(*) FROM android_garbage_collection_events
              WHERE gc_type = "native_alloc" AND gc_ts > (SELECT COALESCE(MIN(ts), 0)
                FROM thread_slice WHERE name GLOB "*android.intent.action.USER_UNLOCKED*"
                ORDER BY ts ASC LIMIT 1 )
            ),
            'explicit_gc_count', (SELECT COUNT(*) FROM android_garbage_collection_events
              WHERE gc_type = "explicit_gc" AND gc_ts > (SELECT COALESCE(MIN(ts), 0)
                FROM thread_slice WHERE name GLOB "*android.intent.action.USER_UNLOCKED*"
                ORDER BY ts ASC LIMIT 1 )
            ),
            'alloc_gc_count', (SELECT COUNT(*) FROM android_garbage_collection_events
              WHERE gc_type = "alloc_gc" AND gc_ts > (SELECT COALESCE(MIN(ts), 0)
                FROM thread_slice WHERE name GLOB "*android.intent.action.USER_UNLOCKED*"
                ORDER BY ts ASC LIMIT 1 )
            ),
            'mb_per_ms_of_gc', (SELECT SUM(reclaimed_mb)/SUM(gc_running_dur/1e6) AS mb_per_ms_dur
              FROM android_garbage_collection_events
              WHERE gc_ts > (SELECT COALESCE(MIN(ts), 0) FROM thread_slice
                WHERE name GLOB "*android.intent.action.USER_UNLOCKED*" ORDER BY ts
                ASC LIMIT 1 )
            )
        ))
    )
);
