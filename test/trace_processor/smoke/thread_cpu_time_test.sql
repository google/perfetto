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
SELECT
  tid,
  pid,
  thread.name AS threadName,
  process.name AS processName,
  total_dur AS totalDur
FROM
  thread
LEFT JOIN process USING(upid)
LEFT JOIN
  (SELECT upid, sum(dur) AS total_dur
    FROM sched JOIN thread USING(utid)
    WHERE dur != -1
    GROUP BY upid
  ) USING(upid)
WHERE utid != 0
ORDER BY total_dur DESC, pid, tid;
