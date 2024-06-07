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

INCLUDE PERFETTO MODULE android.process_metadata;

DROP VIEW IF EXISTS process_metadata_table;
CREATE PERFETTO VIEW process_metadata_table AS
SELECT android_process_metadata.*, pid FROM android_process_metadata
JOIN process USING(upid);

DROP VIEW IF EXISTS uid_package_count;
CREATE PERFETTO VIEW uid_package_count AS
SELECT * FROM _uid_package_count;

DROP VIEW IF EXISTS process_metadata;
CREATE PERFETTO VIEW process_metadata AS
SELECT
  upid,
  NULL_IF_EMPTY(AndroidProcessMetadata(
    'name', process_name,
    'uid', uid,
    'pid', pid,
    'package', NULL_IF_EMPTY(AndroidProcessMetadata_Package(
      'package_name', package_name,
      'apk_version_code', version_code,
      'debuggable', debuggable
    ))
  )) AS metadata
FROM process_metadata_table;

-- Given a process name, return if it is debuggable.
CREATE OR REPLACE PERFETTO FUNCTION is_process_debuggable(process_name STRING)
RETURNS BOOL AS
SELECT p.debuggable
FROM process_metadata_table p
WHERE p.process_name = $process_name
LIMIT 1;
