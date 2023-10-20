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

CREATE PERFETTO FUNCTION get_durations(process_name STRING)
RETURNS TABLE(uint_sleep_dur LONG, total_dur LONG) AS
SELECT
    SUM(CASE WHEN thread_state.state="D" then thread_state.dur ELSE 0 END) AS uint_sleep_dur,
    SUM(thread_state.dur) as total_dur
FROM android_process_metadata
INNER JOIN thread ON thread.upid=android_process_metadata.upid
INNER JOIN thread_state ON thread.utid=thread_state.utid WHERE android_process_metadata.process_name=$process_name;

DROP VIEW IF EXISTS android_boot_output;
CREATE VIEW android_boot_output AS
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
        FROM slice where name="LauncherColdStartup")
);
