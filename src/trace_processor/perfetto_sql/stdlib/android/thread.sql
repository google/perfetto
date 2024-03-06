--
-- Copyright 2023 The Android Open Source Project
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

CREATE PERFETTO FUNCTION _thread_prefix(thread_name STRING)
RETURNS STRING AS
SELECT STR_SPLIT(STR_SPLIT(STR_SPLIT(STR_SPLIT($thread_name, "-", 0), "[", 0), ":", 0), " ", 0);

-- Per process stats of threads created in a process
CREATE PERFETTO FUNCTION _android_thread_creation_spam(
  -- Minimum duration between creating and destroying a thread before their the
  -- thread creation event is considered. If NULL, considers all thread creations.
  min_thread_dur FLOAT,
  -- Sliding window duration for counting the thread creations. Each window
  -- starts at the first thread creation per <process, thread_name_prefix>.
  sliding_window_dur FLOAT)
RETURNS TABLE(
  -- Process name creating threads.
  process_name STRING,
  -- Process pid creating threads.
  pid INT,
  -- String prefix of thread names created.
  thread_name_prefix STRING,
  -- Max number of threads created within a time window.
  max_count_per_sec INT
) AS
WITH
x AS (
  SELECT
    pid,
    upid,
    _THREAD_PREFIX(thread.name) AS thread_name_prefix,
    process.name AS process_name,
    COUNT(thread.start_ts)
      OVER (
        PARTITION BY upid, thread.name
        ORDER BY thread.start_ts
        RANGE BETWEEN CURRENT ROW AND CAST($sliding_window_dur AS INT64) FOLLOWING
      ) AS count
  FROM thread
  JOIN process
    USING (upid)
  WHERE
    ($min_thread_dur AND (thread.end_ts - thread.start_ts) <= $min_thread_dur)
    OR $min_thread_dur IS NULL
)
SELECT process_name, pid, thread_name_prefix, MAX(count) AS max_count_per_sec
FROM x
GROUP BY upid, thread_name_prefix
HAVING max_count_per_sec > 0
ORDER BY count DESC;
