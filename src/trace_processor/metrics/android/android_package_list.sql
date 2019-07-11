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

-- Generate a table mapping package names to their attributes
CREATE VIEW package_args AS
SELECT args.arg_set_id, args.key, args.string_value, args.int_value
FROM metadata JOIN args ON metadata.int_value = args.arg_set_id
WHERE metadata.name = 'android_packages_list';

CREATE TABLE package_list(
  package_name TEXT PRIMARY KEY,
  uid INT,
  version_code INT
);

INSERT INTO package_list
SELECT names.name, uids.uid, versions.version
FROM
  (SELECT arg_set_id, string_value name FROM package_args WHERE key = 'name')
    AS names
  JOIN (SELECT arg_set_id, int_value uid FROM package_args WHERE key = 'uid')
    AS uids USING (arg_set_id)
  JOIN (SELECT arg_set_id, int_value version FROM package_args WHERE key = 'version_code')
    AS versions USING (arg_set_id);

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
