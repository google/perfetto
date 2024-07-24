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

-- Count packages by package UID.
CREATE PERFETTO TABLE _uid_package_count AS
SELECT uid, COUNT(1) AS cnt
FROM package_list
GROUP BY 1;

CREATE PERFETTO FUNCTION _android_package_for_process(
  uid INT,
  uid_count INT,
  process_name STRING
)
RETURNS TABLE(
  package_name STRING,
  version_code INT,
  debuggable BOOL
)
AS
WITH min_distance AS (
  SELECT
    -- SQLite allows omitting the group-by for the MIN: the other columns
    -- will match the row with the minimum value.
    MIN(LENGTH($process_name) - LENGTH(package_name)),
    package_name,
    version_code,
    debuggable
  FROM package_list
  WHERE (
    (
      $uid = uid
      AND (
        -- unique match
        $uid_count = 1
        -- or process name is a prefix the package name
        OR $process_name GLOB package_name || '*'
      )
    )
    OR
    (
      -- isolated processes can only be matched based on the name
      $uid >= 90000 AND $uid < 100000
      AND STR_SPLIT($process_name, ':', 0) GLOB package_name || '*'
    )
  )
)
SELECT package_name, version_code, debuggable
FROM min_distance;

-- Data about packages running on the process.
CREATE PERFETTO TABLE android_process_metadata(
  -- Process upid.
  upid INT,
  -- Process pid.
  pid INT,
  -- Process name.
  process_name STRING,
  -- Android app UID.
  uid INT,
  -- Whether the UID is shared by multiple packages.
  shared_uid BOOL,
  -- Name of the packages running in this process.
  package_name STRING,
  -- Package version code.
  version_code INT,
  -- Whether package is debuggable.
  debuggable INT
) AS
SELECT
  process.upid,
  process.pid,
  -- workaround for b/169226092: the bug has been fixed it Android T, but
  -- we support ingesting traces from older Android versions.
  CASE
    -- cmdline gets rewritten after fork, if these are still there we must
    -- have seen a racy capture.
    WHEN length(process.name) = 15 AND (
      process.cmdline IN ('zygote', 'zygote64', '<pre-initialized>')
      OR process.cmdline GLOB '*' || process.name)
      THEN process.cmdline
    ELSE process.name
  END AS process_name,
  process.android_appid AS uid,
  CASE WHEN _uid_package_count.cnt > 1 THEN TRUE ELSE NULL END AS shared_uid,
  plist.package_name,
  plist.version_code,
  plist.debuggable
FROM process
LEFT JOIN _uid_package_count ON process.android_appid = _uid_package_count.uid
LEFT JOIN _android_package_for_process(
  process.android_appid, _uid_package_count.cnt, process.name
) AS plist
ORDER BY upid;
