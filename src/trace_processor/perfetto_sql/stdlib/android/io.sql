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

-- TODO(b/329344794): Rewrite to fetch data from other tables than `raw`.

-- Aggregates f2fs IO and latency stats by counter name.
CREATE PERFETTO VIEW _android_io_f2fs_counter_stats(
  -- Counter name on which all the other values are aggregated on.
  name STRING,
  -- Sum of all counter values for the counter name.
  sum DOUBLE,
  -- Max of all counter values for the counter name.
  max DOUBLE,
  -- Min of all counter values for the counter name.
  min DOUBLE,
  -- Duration between the first and last counter value for the counter name.
  dur INT,
  -- Count of all the counter values for the counter name.
  count INT,
  -- Avergate of all the counter values for the counter name.
  avg DOUBLE
) AS
SELECT
  STR_SPLIT(counter_track.name, '].', 1) AS name,
  SUM(counter.value) AS sum,
  MAX(counter.value) AS max,
  MIN(counter.value) AS min,
  MAX(ts) - MIN(ts) AS dur,
  COUNT(ts) AS count,
  AVG(counter.value) AS avg
FROM counter
JOIN counter_track
  ON counter_track.id = counter.track_id AND counter_track.name GLOB '*f2fs*'
GROUP BY name
ORDER BY sum DESC;

-- Aggregates f2fs_write stats by inode and thread.
CREATE PERFETTO VIEW _android_io_f2fs_write_stats(
  -- Utid of the thread.
  utid INT,
  -- Tid of the thread.
  tid INT,
  -- Name of the thread.
  thread_name STRING,
  -- Upid of the process.
  upid INT,
  -- Pid of the process.
  pid INT,
  -- Name of the thread.
  process_name STRING,
  -- Inode number of the file being written.
  ino INT,
  -- Device node number of the file being written.
  dev INT,
  -- Total number of bytes written on this file by the |utid|.
  bytes INT,
  -- Total count of write requests for this file.
  write_count INT
) AS
WITH
  f2fs_write_end AS (
    SELECT
      *,
      EXTRACT_ARG(arg_set_id, 'len') AS len,
      EXTRACT_ARG(arg_set_id, 'dev') AS dev,
      EXTRACT_ARG(arg_set_id, 'ino') AS ino,
      EXTRACT_ARG(arg_set_id, 'copied') AS copied
    FROM raw
    WHERE name GLOB 'f2fs_write_end*'
  )
SELECT
  thread.utid,
  thread.tid,
  thread.name AS thread_name,
  process.upid,
  process.pid,
  process.name AS process_name,
  f.ino,
  f.dev,
  SUM(copied) AS bytes,
  COUNT(len) AS write_count
FROM f2fs_write_end f
JOIN thread
  USING (utid)
JOIN process
  USING (upid)
GROUP BY utid, ino, dev
ORDER BY bytes DESC;

-- Aggregates f2fs write stats. Counts distinct datapoints, total write operations,
-- and bytes written
CREATE PERFETTO VIEW _android_io_f2fs_aggregate_write_stats(
  -- Total number of writes in the trace.
  total_write_count INT,
  -- Number of distinct processes.
  distinct_processes INT,
  -- Total number of bytes written.
  total_bytes_written INT,
  -- Count of distinct devices written to.
  distinct_device_count INT,
  -- Count of distinct inodes written to.
  distinct_inode_count INT,
  -- Count of distinct threads writing.
  distinct_thread_count INT
) AS
select SUM(write_count) as total_write_count,
      COUNT(DISTINCT pid) distinct_processes,
      SUM(bytes) as total_bytes_written,
      COUNT(DISTINCT dev) as distinct_device_count,
      COUNT(DISTINCT ino) distinct_inode_count,
      COUNT(DISTINCT tid) distinct_thread_count
from _android_io_f2fs_write_stats;