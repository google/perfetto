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

SELECT IMPORT('android.startup.startups');

-- Helper function to build a Slice proto from a duration.
SELECT CREATE_FUNCTION('STARTUP_SLICE_PROTO(dur INT)', 'PROTO', '
  SELECT AndroidStartupMetric_Slice(
    "dur_ns", $dur,
    "dur_ms", $dur / 1e6
  )
');

-- View containing all the slices for all launches. Generally, this view
-- should not be used. Instead, one of the helper functions below which wrap
-- this view should be used.
DROP VIEW IF EXISTS thread_slices_for_all_launches;
CREATE VIEW thread_slices_for_all_launches AS
SELECT * FROM android_thread_slices_for_all_startups;


-- Given a launch id and GLOB for a slice name,
-- summing the slice durations across the whole startup.
SELECT CREATE_FUNCTION(
  'ANDROID_SUM_DUR_FOR_STARTUP_AND_SLICE(startup_id LONG, slice_name STRING)',
  'INT',
  '
    SELECT SUM(slice_dur)
    FROM android_thread_slices_for_all_startups
    WHERE startup_id = $startup_id AND slice_name GLOB $slice_name
  '
);

-- Given a launch id and GLOB for a slice name, returns the startup slice proto,
-- summing the slice durations across the whole startup.
SELECT CREATE_FUNCTION(
  'DUR_SUM_SLICE_PROTO_FOR_LAUNCH(startup_id LONG, slice_name STRING)',
  'PROTO',
  '
    SELECT NULL_IF_EMPTY(
      STARTUP_SLICE_PROTO(
        ANDROID_SUM_DUR_FOR_STARTUP_AND_SLICE($startup_id, $slice_name)
      )
    )
  '
);

-- Same as |DUR_SUM_SLICE_PROTO_FOR_LAUNCH| except only counting slices happening
-- on the main thread.
SELECT CREATE_FUNCTION(
  'DUR_SUM_MAIN_THREAD_SLICE_PROTO_FOR_LAUNCH(startup_id LONG, slice_name STRING)',
  'PROTO',
  '
    SELECT NULL_IF_EMPTY(
      STARTUP_SLICE_PROTO(
        ANDROID_SUM_DUR_ON_MAIN_THREAD_FOR_STARTUP_AND_SLICE($startup_id, $slice_name)
      )
    )
  '
);

-- Given a launch id and GLOB for a slice name, returns the startup slice proto by
-- taking the duration between the start of the launch and start of the slice.
-- If multiple slices match, picks the latest one which started during the launch.
SELECT CREATE_FUNCTION(
  'LAUNCH_TO_MAIN_THREAD_SLICE_PROTO(startup_id INT, slice_name STRING)',
  'PROTO',
  '
    SELECT NULL_IF_EMPTY(STARTUP_SLICE_PROTO(MAX(slice_ts) - startup_ts))
    FROM android_thread_slices_for_all_startups s
    JOIN thread t USING (utid)
    WHERE
      s.slice_name GLOB $slice_name AND
      s.startup_id = $startup_id AND
      s.is_main_thread AND
      (t.end_ts IS NULL OR t.end_ts >= s.startup_ts_end)
  '
);

-- Given a lauch id, returns the total time spent in GC
SELECT CREATE_FUNCTION(
  'TOTAL_GC_TIME_BY_LAUNCH(startup_id LONG)',
  'INT',
  '
    SELECT SUM(slice_dur)
        FROM android_thread_slices_for_all_startups slice
        WHERE
          slice.startup_id = $startup_id AND
          (
            slice_name GLOB "*semispace GC" OR
            slice_name GLOB "*mark sweep GC" OR
            slice_name GLOB "*concurrent copying GC"
          )
  '
);

-- Given a launch id and package name, returns if baseline or cloud profile is missing.
SELECT CREATE_FUNCTION(
  'MISSING_BASELINE_PROFILE_FOR_LAUNCH(startup_id LONG, pkg_name STRING)',
  'BOOL',
  '
    SELECT (COUNT(slice_name) > 0)
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
      AND STR_SPLIT(STR_SPLIT(slice_name, " filter=", 1), " reason=", 0) != "speed-profile"
  '
);

SELECT CREATE_FUNCTION(
  'RUN_FROM_APK_FOR_LAUNCH(launch_id LONG)',
  'BOOL',
  '
    SELECT EXISTS(
      SELECT slice_name
      FROM (
        SELECT *
        FROM ANDROID_SLICES_FOR_STARTUP_AND_SLICE_NAME(
          $launch_id,
          "location=* status=* filter=* reason=*"
        )
      )
      WHERE
        STR_SPLIT(STR_SPLIT(slice_name, " filter=", 1), " reason=", 0)
          GLOB ("*" || "run-from-apk" || "*")
    )
  '
);

SELECT CREATE_VIEW_FUNCTION(
  'BINDER_TRANSACTION_REPLY_SLICES_FOR_LAUNCH(startup_id INT, threshold DOUBLE)',
  'name STRING',
  '
    SELECT reply.name AS name
    FROM ANDROID_BINDER_TRANSACTION_SLICES_FOR_STARTUP($startup_id, $threshold) request
    JOIN following_flow(request.id) arrow
    JOIN slice reply ON reply.id = arrow.slice_in
    WHERE reply.dur > $threshold AND request.is_main_thread
  '
);

-- Given a launch id, return if unlock is running by systemui during the launch.
SELECT CREATE_FUNCTION(
  'IS_UNLOCK_RUNNING_DURING_LAUNCH(startup_id LONG)',
  'BOOL',
  '
    SELECT EXISTS(
      SELECT slice.name
      FROM slice, android_startups launches
      JOIN thread_track ON slice.track_id = thread_track.id
      JOIN thread USING(utid)
      JOIN process USING(upid)
      WHERE launches.startup_id = $startup_id
      AND slice.name = "KeyguardUpdateMonitor#onAuthenticationSucceeded"
      AND process.name = "com.android.systemui"
      AND slice.ts >= launches.ts
      AND (slice.ts + slice.dur) <= launches.ts_end
    )
  '
);
