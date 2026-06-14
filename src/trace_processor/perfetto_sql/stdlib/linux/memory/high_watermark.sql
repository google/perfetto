--
-- Copyright 2024 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the 'License');
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an 'AS IS' BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- NOTE (psqlnext): the `counters.intervals` module is DELETED —
-- `counter_leading_intervals!` is `INTERVALS FROM EVENTS` (+`MERGE
-- CONSECUTIVE BY value`).

INCLUDE PERFETTO MODULE linux.memory.process;

CREATE PERFETTO PIPELINE _memory_rss_high_watermark_per_process_table MATERIALIZED AS
-- The running max of rss per process becomes the counter value; equal-valued
-- runs (the watermark holding flat) coalesce into single intervals.
SUBPIPELINE high_watermark_as_counter AS (
  FROM _memory_rss_and_swap_per_process_table
  |> SELECT
       ts,
       max(coalesce(file_rss, 0) + coalesce(anon_rss, 0) + coalesce(shmem_rss, 0))
         OVER (PARTITION BY upid ORDER BY ts) AS value,
       -- `track_id` aliases `upid` so leading-interval lanes are per-process.
       upid AS track_id
)
INTERVALS FROM EVENTS high_watermark_as_counter PER track_id CLOSING LAST AT (trace_end())
|> INTERVAL MERGE CONSECUTIVE BY value
|> SELECT ts, dur, track_id AS upid, cast_int!(value) AS rss_high_watermark;

-- For each process fetches the memory high watermark until or during
-- timestamp.
CREATE PERFETTO PIPELINE memory_rss_high_watermark_per_process(
  -- Timestamp
  ts TIMESTAMP,
  -- Duration
  dur DURATION,
  -- Upid of the process
  upid JOINID(process.id),
  -- Pid of the process
  pid LONG,
  -- Name of the process
  process_name STRING,
  -- Maximum `rss` value until now
  rss_high_watermark LONG
)
AS
FROM _memory_rss_high_watermark_per_process_table
|> JOIN process USING (upid)
|> SELECT ts, dur, upid, pid, name AS process_name, rss_high_watermark;
