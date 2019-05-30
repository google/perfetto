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

-- Sched view per process
CREATE VIEW per_process_cpu AS
SELECT process.upid AS upid, ts, dur
FROM sched
JOIN thread USING(utid)
JOIN process USING(upid);

-- CPU usage during the activity launch.
CREATE TABLE launch_cpu_per_process_type AS
SELECT
  id AS launch_id,
  per_process_cpu.upid IN (
    SELECT upid FROM launch_processes AS lp WHERE lp.launch_id = launches.id
  ) AS is_launch_process,
  SUM(per_process_cpu.dur) AS dur
FROM launches
JOIN per_process_cpu ON (
  per_process_cpu.ts BETWEEN launches.ts AND launches.ts + launches.dur)
GROUP BY 1, 2;

CREATE VIEW launch_cpu AS
SELECT
  launch_id,
  other_process.dur AS other_process_dur,
  launch_process.dur AS launch_process_dur,
  1.0 * IFNULL(other_process.dur, 0) / launch_process.dur AS cpu_ratio
FROM (
  SELECT * FROM launch_cpu_per_process_type
  WHERE is_launch_process = 1
) AS launch_process
LEFT JOIN (
  SELECT * FROM launch_cpu_per_process_type
  WHERE is_launch_process = 0
) AS other_process
USING (launch_id);
