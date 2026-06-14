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

-- Extract name of the device based on metadata from the trace.
CREATE PERFETTO PIPELINE android_device_name(
  -- Device name.
  name STRING,
  -- Machine identifier
  machine_id JOINID(machine.id)
) MATERIALIZED AS
-- Example android_build_fingerprint:
-- Android/aosp_raven/raven:VanillaIceCream/UDC/11197703:userdebug/test-keys
-- The device name is the portion after the second slash and before the colon.
FROM machine
|> WHERE android_build_fingerprint IS NOT NULL
|> EXTEND substr(
     android_build_fingerprint,
     instr(android_build_fingerprint, '/') + 1
   ) AS after_first_slash
|> EXTEND substr(after_first_slash, instr(after_first_slash, '/') + 1) AS after_second_slash
|> SELECT
     substr(after_second_slash, 0, instr(after_second_slash, ':')) AS name,
     id AS machine_id;
