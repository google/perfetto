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


INSERT INTO internal_all_startups
SELECT
  "maxsdk28",
  ROW_NUMBER() OVER(ORDER BY ts) AS startup_id,
  le.ts,
  le.ts_end AS ts_end,
  le.ts_end - le.ts AS dur,
  package_name AS package,
  NULL AS startup_type
FROM internal_startup_events le
ORDER BY ts;

