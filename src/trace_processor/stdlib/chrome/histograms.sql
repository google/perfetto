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

DROP VIEW IF EXISTS chrome_histograms;

-- A helper view on top of the histogram events emitted by Chrome.
-- Requires "disabled-by-default-histogram_samples" Chrome category.
--
-- @column name          The name of the histogram.
-- @column value         The value of the histogram sample.
-- @column ts            Alias of |slice.ts|.
-- @column thread_name   Thread name.
-- @column utid          Utid of the thread.
-- @column tid           Tid of the thread.
-- @column process_name  Process name.
-- @column upid          Upid of the process.
-- @column pid           Pid of the process.
CREATE VIEW chrome_histograms AS
SELECT
  extract_arg(slice.arg_set_id, "chrome_histogram_sample.name") as name,
  extract_arg(slice.arg_set_id, "chrome_histogram_sample.sample") as value,
  ts,
  thread.name as thread_name,
  thread.utid as utid,
  thread.tid as tid,
  process.name as process_name,
  process.upid as upid,
  process.pid as pid
FROM slice
JOIN thread_track ON thread_track.id = slice.track_id
JOIN thread USING (utid)
JOIN process USING (upid)
WHERE
  slice.name = "HistogramSample"
  AND category = "disabled-by-default-histogram_samples";