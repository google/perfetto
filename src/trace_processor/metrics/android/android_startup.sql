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
SELECT RUN_METRIC('android/android_task_state.sql');

-- Slices for forked processes. Never present in hot starts.
-- Prefer this over process start_ts, since the process might have
-- been preforked.
CREATE VIEW zygote_fork_slices AS
SELECT slices.ts, slices.dur, STR_SPLIT(slices.name, ": ", 1) AS process_name
FROM slices WHERE name LIKE 'Start proc: %';

CREATE TABLE zygote_forks_by_id AS
SELECT
  launches.id,
  zygote_fork_slices.ts,
  zygote_fork_slices.dur
FROM zygote_fork_slices
JOIN launches
ON (launches.ts < zygote_fork_slices.ts
    AND zygote_fork_slices.ts + zygote_fork_slices.dur < launches.ts_end
    AND zygote_fork_slices.process_name = launches.package
);

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

CREATE VIRTUAL TABLE main_thread_state
USING SPAN_JOIN(
  launch_main_threads PARTITIONED utid,
  task_state PARTITIONED utid);

CREATE VIEW launch_by_thread_state AS
SELECT launch_id, state, SUM(dur) AS dur
FROM main_thread_state
GROUP BY 1, 2;

CREATE VIEW startup_view AS
SELECT
  AndroidStartupMetric_Startup(
    'startup_id', launches.id,
    'package_name', launches.package,
    'process_name', (
      SELECT name FROM process
      WHERE upid IN (
        SELECT upid FROM launch_processes
        WHERE launch_id = launches.id
        LIMIT 1)
    ),
    'zygote_new_process', EXISTS(SELECT TRUE FROM zygote_forks_by_id WHERE id = launches.id),
    'activity_hosting_process_count', (
      SELECT COUNT(1) FROM launch_processes WHERE launch_id = launches.id
    ),
    'to_first_frame', AndroidStartupMetric_ToFirstFrame(
      'dur_ns', launches.dur,
      'main_thread_by_task_state', TaskStateBreakdown(
        'running_dur_ns', IFNULL(
            (
            SELECT dur FROM launch_by_thread_state
            WHERE launch_id = launches.id AND state = 'running'
            ), 0),
        'runnable_dur_ns', IFNULL(
            (
            SELECT dur FROM launch_by_thread_state
            WHERE launch_id = launches.id AND state = 'runnable'
            ), 0),
        'uninterruptible_sleep_dur_ns', IFNULL(
            (
            SELECT dur FROM launch_by_thread_state
            WHERE launch_id = launches.id AND state = 'uninterruptible'
            ), 0),
        'interruptible_sleep_dur_ns', IFNULL(
            (
            SELECT dur FROM launch_by_thread_state
            WHERE launch_id = launches.id AND state = 'interruptible'
            ), 0)
      ),
      'other_processes_spawned_count', (
        SELECT COUNT(1) FROM process
        WHERE (process.name IS NULL OR process.name != launches.package)
        AND process.start_ts BETWEEN launches.ts AND launches.ts + launches.dur
      )
    )
  ) as startup
FROM launches;

CREATE VIEW android_startup_output AS
SELECT
  AndroidStartupMetric(
    'startup', (
      SELECT RepeatedField(startup) FROM startup_view
    )
  );
