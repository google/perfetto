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
--

INCLUDE PERFETTO MODULE android.startup.startups;

-- Collect the last ts for user switch event.
-- The metric should represent time elapsed between
-- the latest user start and the latest carlauncher startup.
DROP VIEW IF EXISTS auto_multiuser_events;
CREATE PERFETTO VIEW auto_multiuser_events AS
SELECT
  user_start_time_ns AS event_start_time_ns,
  launcher_end_time_ns AS event_end_time_ns
FROM
  (
    SELECT MAX(slice.ts) as user_start_time_ns
    FROM slice
    WHERE (
        slice.name GLOB "UserController.startUser*"
        AND slice.name NOT GLOB "UserController.startUser-10*"
    )
  ),
  (
    SELECT ts_end AS launcher_end_time_ns
    FROM android_startups
    WHERE (package = 'com.android.car.carlauncher')
  );

-- Precompute user switch duration time.
-- Take only positive duration values(user start ts < carlauncher start ts)
-- If there are potential duplicates in carlauncher startup,
-- take the smallest value. It represents the closest carlaucnher startup
DROP TABLE IF EXISTS android_auto_multiuser_timing;
CREATE PERFETTO TABLE android_auto_multiuser_timing AS
SELECT
  cast_int!((event_end_time_ns - event_start_time_ns) / 1e6 + 0.5) as duration_ms
FROM
  auto_multiuser_events
WHERE duration_ms > 0
ORDER BY duration_ms ASC
LIMIT 1;

DROP VIEW IF EXISTS android_auto_multiuser_output;
CREATE PERFETTO VIEW android_auto_multiuser_output AS
SELECT AndroidMultiuserMetric (
    'user_switch', AndroidMultiuserMetric_EventData(
        'duration_ms', (
            SELECT duration_ms FROM android_auto_multiuser_timing
        )
    )
);