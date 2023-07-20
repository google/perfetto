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

-- Aggregates f2fs IO and latency stats by counter name.
--
-- @column counter_name   Counter name on which all the other values are aggregated on.
-- @column counter_sum    Sum of all counter values for the counter name.
-- @column counter_max    Max of all counter values for the counter name.
-- @column counter_min    Min of all counter values for the counter name.
-- @column counter_dur    Duration between the first and last counter value for the counter name.
-- @column counter_count  Count of all the counter values for the counter name.
-- @column counter_avg    Avergate of all the counter values for the counter name.
CREATE VIEW android_io_f2fs_counter_stats AS
SELECT
  STR_SPLIT(counter_track.name, '].', 1) AS counter_name,
  SUM(counter.value) AS counter_sum,
  MAX(counter.value) AS counter_max,
  MIN(counter.value) AS counter_min,
  MAX(ts) - MIN(ts) AS counter_dur,
  COUNT(ts) AS counter_count,
  AVG(counter.value) AS counter_avg
FROM counter
JOIN counter_track
  ON counter_track.id = counter.track_id AND counter_track.name LIKE '%f2fs%'
GROUP BY counter_name
ORDER BY counter_sum DESC;

-- Aggregates f2fs_write stats by inode and thread.
--
-- @column utid          Utid of the thread.
-- @column tid           Tid of the thread.
-- @column thread_name   Name of the thread.
-- @column upid          Upid of the process.
-- @column pid           Pid of the process.
-- @column process_name  Name of the thread.
-- @column ino           Inode number of the file being written.
-- @column dev           Device node number of the file being written.
-- @column bytes         Total number of bytes written on this file by the |utid|.
-- @column write_count   Total count of write requests for this file.
CREATE VIEW android_io_f2fs_write_stats AS
WITH
  f2fs_write_end AS (
    SELECT
      *,
      EXTRACT_ARG(arg_set_id, 'len') AS len,
      EXTRACT_ARG(arg_set_id, 'dev') AS dev,
      EXTRACT_ARG(arg_set_id, 'ino') AS ino,
      EXTRACT_ARG(arg_set_id, 'copied') AS copied
    FROM raw
    WHERE name LIKE 'f2fs_write_end%'
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
