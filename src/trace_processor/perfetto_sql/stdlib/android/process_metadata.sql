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
CREATE PERFETTO TABLE internal_uid_package_count AS
SELECT uid, COUNT(1) AS cnt
FROM package_list
GROUP BY 1;

-- Data about packages running on the process.
--
-- @column upid         Process upid.
-- @column process_name Process name.
-- @column package_name Name of the packages running in this process.
-- @column version_code Package version code.
-- @column debuggable   Whether package is debuggable.
CREATE PERFETTO TABLE android_process_metadata AS
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
  CASE WHEN internal_uid_package_count.cnt > 1 THEN TRUE ELSE NULL END AS shared_uid,
  plist.package_name,
  plist.version_code,
  plist.debuggable
FROM process
LEFT JOIN internal_uid_package_count ON process.android_appid = internal_uid_package_count.uid
LEFT JOIN package_list plist
  ON (
    (
      process.android_appid = plist.uid
      AND internal_uid_package_count.uid = plist.uid
      AND (
        -- unique match
        internal_uid_package_count.cnt = 1
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
