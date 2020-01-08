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

SELECT RUN_METRIC('android/android_package_list.sql');

-- Create a view of the process with the app ID as defined in
-- //frameworks/base/core/java/android/os/UserHandle.java
-- TODO: move this to the trace processor once the table migration is complete.
CREATE VIEW IF NOT EXISTS proc_uid AS
SELECT upid, name, uid % 100000 AS uid
FROM process;

DROP TABLE IF EXISTS uid_package_count;

CREATE TABLE uid_package_count AS
SELECT uid, COUNT(1) AS cnt
FROM package_list
GROUP BY 1;

DROP TABLE IF EXISTS process_metadata_table;

CREATE TABLE process_metadata_table AS
SELECT
  proc_uid.upid,
  proc_uid.name AS process_name,
  proc_uid.uid,
  CASE WHEN uid_package_count.cnt > 1 THEN TRUE ELSE NULL END AS shared_uid,
  plist.package_name,
  plist.version_code,
  plist.debuggable
FROM proc_uid
LEFT JOIN uid_package_count USING (uid)
LEFT JOIN package_list plist
ON (
  proc_uid.uid = plist.uid
  AND uid_package_count.uid = plist.uid
  AND (
    -- unique match
    uid_package_count.cnt = 1
    -- or process name starts with the package name
    OR proc_uid.name LIKE plist.package_name || '%')
  );

DROP VIEW IF EXISTS process_metadata;

CREATE VIEW IF NOT EXISTS process_metadata AS
WITH upid_packages AS (
  SELECT
  upid,
  RepeatedField(AndroidProcessMetadata_Package(
    'package_name', package_list.package_name,
    'apk_version_code', package_list.version_code,
    'debuggable', package_list.debuggable
  )) packages_for_uid
  FROM proc_uid
  JOIN package_list USING (uid)
  GROUP BY upid
)
SELECT
  upid,
  AndroidProcessMetadata(
    'name', process_name,
    'uid', uid,
    'package', AndroidProcessMetadata_Package(
      'package_name', package_name,
      'apk_version_code', version_code,
      'debuggable', debuggable
    ),
    'packages_for_uid', packages_for_uid
  ) AS metadata
FROM process_metadata_table
LEFT JOIN upid_packages USING (upid);
