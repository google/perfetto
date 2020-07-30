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

SELECT RUN_METRIC('android/android_proxy_power.sql') AS suppress_query_output;

-- The test trace doesn't contain metadata necessary to determine the device
-- name, so we create a table with the name directly.
DROP VIEW device;

CREATE TABLE device (name STRING);

INSERT INTO device VALUES ('walleye');

-- Select the top 10 threads by power usage.
SELECT
  utid,
  SUM(dur * COALESCE(power_ma, 0) / 1e9) AS power_mas
FROM power_per_thread
GROUP BY utid
ORDER BY power_mas DESC
LIMIT 10;
