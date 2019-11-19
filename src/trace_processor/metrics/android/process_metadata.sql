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

DROP TABLE IF EXISTS process_metadata;

CREATE TABLE process_metadata AS
SELECT
  process.upid,
  AndroidProcessMetadata(
    'name', process.name,
    'uid', uid,
    'package_name', plist.package_name,
    'apk_version_code', plist.version_code,
    'debuggable', plist.debuggable
  ) AS metadata
FROM process LEFT JOIN package_list plist USING (uid);
