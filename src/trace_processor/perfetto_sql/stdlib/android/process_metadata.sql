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

-- Data about packages running on the process.
CREATE PERFETTO TABLE android_process_metadata(
  -- Process upid.
  upid INT,
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
LEFT JOIN package_list plist
  ON (
    (
      process.android_appid = plist.uid
      AND _uid_package_count.uid = plist.uid
      AND (
        -- unique match
        _uid_package_count.cnt = 1
        -- or process name starts with the package name
        OR process.name GLOB plist.package_name || '*')
    )
    OR
    (
      -- isolated processes can only be matched based on the name prefix
      process.android_appid >= 90000 AND process.android_appid < 100000
      AND STR_SPLIT(process.name, ':', 0) GLOB plist.package_name || '*'
    )
  );
