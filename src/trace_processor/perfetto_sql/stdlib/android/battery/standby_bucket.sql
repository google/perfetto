-- Copyright 2025 The Android Open Source Project
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

-- Provides unified access to Android App Standby Bucket events from StatsD.
--
-- Suggested minimal config:
--
-- data_sources: {
--     config: {
--         name: "android.statsd"
--         statsd_config: {
--             atom_id: 258  # AppStandbyBucketChanged
--         }
--     }
-- }

-- Table for raw App Standby Bucket change events from StatsD
CREATE PERFETTO TABLE android_standby_bucket_changes(
  -- Timestamp of standby bucket change.
  ts TIMESTAMP,
  -- Package name of the app.
  package_name STRING,
  -- User ID of the app.
  user_id LONG,
  -- Standby bucket name.
  bucket STRING,
  -- Main reason for bucket change.
  main_reason STRING,
  -- Sub reason for bucket change.
  sub_reason LONG
)
AS
SELECT
  s.ts,
  extract_arg(s.arg_set_id, 'app_standby_bucket_changed.package_name') AS package_name,
  extract_arg(s.arg_set_id, 'app_standby_bucket_changed.user_id') AS user_id,
  extract_arg(s.arg_set_id, 'app_standby_bucket_changed.bucket') AS bucket,
  extract_arg(s.arg_set_id, 'app_standby_bucket_changed.main_reason') AS main_reason,
  extract_arg(s.arg_set_id, 'app_standby_bucket_changed.sub_reason') AS sub_reason
FROM slice AS s
JOIN track AS t
  ON s.track_id = t.id
WHERE
  t.name = 'Statsd Atoms'
  AND s.name = 'app_standby_bucket_changed';

-- View to get standby bucket intervals for each package
CREATE PERFETTO VIEW android_standby_bucket(
  -- Timestamp of standby bucket change.
  ts TIMESTAMP,
  -- Duration of standby bucket state.
  dur DURATION,
  -- Package name of the app.
  package_name STRING,
  -- User ID of the app.
  user_id LONG,
  -- Standby bucket name.
  bucket STRING,
  -- Main reason for bucket change.
  main_reason STRING,
  -- Sub reason for bucket change.
  sub_reason LONG
)
AS
SELECT
  ts,
  lead(ts, 1, (SELECT end_ts FROM trace_bounds)) OVER (
    PARTITION BY
      package_name,
      user_id
    ORDER BY ts
  )
  - ts AS dur,
  package_name,
  user_id,
  bucket,
  main_reason,
  sub_reason
FROM android_standby_bucket_changes;
