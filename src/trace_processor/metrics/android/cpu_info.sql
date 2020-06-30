--
-- Copyright 2020 The Android Open Source Project
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

SELECT RUN_METRIC('android/power_profile_data.sql');

CREATE VIEW IF NOT EXISTS core_cluster_per_cpu AS
SELECT DISTINCT cpu, cluster
FROM power_profile pp
WHERE EXISTS (
  SELECT 1 FROM metadata
  WHERE name = 'android_build_fingerprint' AND str_value LIKE '%' || pp.device || '%');

CREATE VIEW IF NOT EXISTS core_type_per_cpu AS
SELECT
  cpu,
  CASE cluster
    WHEN 0 THEN 'little'
    WHEN 1 THEN 'big'
    WHEN 2 THEN 'bigger'
    ELSE 'unknown'
  END core_type
FROM core_cluster_per_cpu;
