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

-- Get distinct packages list
DROP VIEW IF EXISTS package_arg_ids;

CREATE VIEW package_arg_ids AS
SELECT int_value AS arg_set_id
FROM metadata WHERE name = 'android_packages_list';

-- Generate a table mapping package names to their attributes
DROP VIEW IF EXISTS package_args;

CREATE VIEW package_args AS
SELECT arg_set_id, key, string_value, int_value
FROM package_arg_ids JOIN args USING(arg_set_id);

DROP TABLE IF EXISTS package_list;

CREATE TABLE package_list(
  package_name TEXT PRIMARY KEY,
  uid INT,
  version_code INT,
  debuggable INT
);

INSERT OR REPLACE INTO package_list
SELECT names.name, uids.uid, versions.version, debuggable.is_debug
FROM
  (SELECT arg_set_id, string_value name FROM package_args WHERE key = 'name')
    AS names
  JOIN (SELECT arg_set_id, int_value uid FROM package_args WHERE key = 'uid')
    AS uids USING (arg_set_id)
  JOIN (SELECT arg_set_id, int_value version FROM package_args WHERE key = 'version_code')
    AS versions USING (arg_set_id)
  JOIN (SELECT arg_set_id, int_value is_debug FROM package_args WHERE key = 'debuggable')
    AS debuggable USING (arg_set_id);

DROP VIEW IF EXISTS android_package_list_output;

CREATE VIEW android_package_list_output AS
SELECT AndroidPackageList(
  'packages', (
    SELECT RepeatedField(AndroidPackageList_Package(
      'package_name', package_name,
      'uid', uid,
      'version_code', version_code
    )) FROM package_list
  )
);
