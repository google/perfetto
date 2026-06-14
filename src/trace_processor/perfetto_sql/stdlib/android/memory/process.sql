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

INCLUDE PERFETTO MODULE android.oom_adjuster;

INCLUDE PERFETTO MODULE linux.memory.process;

-- Process memory and it's OOM adjuster scores. Detects transitions, each new
-- interval means that either the memory or OOM adjuster score of the process changed.
CREATE PERFETTO PIPELINE memory_oom_score_with_rss_and_swap_per_process(
  -- Timestamp the oom_adj score or memory of the process changed
  ts TIMESTAMP,
  -- Duration until the next oom_adj score or memory change of the process.
  dur DURATION,
  -- oom adjuster score of the process.
  score LONG,
  -- oom adjuster bucket of the process.
  bucket STRING,
  -- Upid of the process having an oom_adj update.
  upid JOINID(process.id),
  -- Name of the process having an oom_adj update.
  process_name STRING,
  -- Pid of the process having an oom_adj update.
  pid LONG,
  -- Slice of the latest oom_adj update in the system_server.
  oom_adj_id JOINID(slice.id),
  -- Timestamp of the latest oom_adj update in the system_server.
  oom_adj_ts TIMESTAMP,
  -- Duration of the latest oom_adj update in the system_server.
  oom_adj_dur DURATION,
  -- Track of the latest oom_adj update in the system_server. Alias of
  -- `track.id`.
  oom_adj_track_id JOINID(track.id),
  -- Thread name of the latest oom_adj update in the system_server.
  oom_adj_thread_name STRING,
  -- Reason for the latest oom_adj update in the system_server.
  oom_adj_reason STRING,
  -- Trigger for the latest oom_adj update in the system_server.
  oom_adj_trigger STRING,
  -- Anon RSS counter value
  anon_rss LONG,
  -- File RSS counter value
  file_rss LONG,
  -- Shared memory RSS counter value
  shmem_rss LONG,
  -- Total RSS value. Sum of `anon_rss`, `file_rss` and `shmem_rss`. Returns
  -- value even if one of the values is NULL.
  rss LONG,
  -- Swap counter value
  swap LONG,
  -- Sum or `anon_rss` and `swap`. Returns value even if one of the values is
  -- NULL.
  anon_rss_and_swap LONG,
  -- Sum or `rss` and `swap`. Returns value even if one of the values is NULL.
  rss_and_swap LONG
)
MATERIALIZED AS
-- The original SPAN_OUTER_JOIN keeps every oom_adj interval, co-fragmenting it
-- with the per-process RSS/swap intervals (null where RSS is absent).
INTERVAL UNION OF (
  android_oom_adj_intervals AS o,
  _memory_rss_and_swap_per_process_table AS m
) PER upid
|> JOIN process AS p USING (upid)
|> SELECT
     ts,
     dur,
     o.score AS score,
     o.bucket AS bucket,
     upid,
     o.process_name AS process_name,
     o.pid AS pid,
     o.oom_adj_id AS oom_adj_id,
     o.oom_adj_ts AS oom_adj_ts,
     o.oom_adj_dur AS oom_adj_dur,
     o.oom_adj_track_id AS oom_adj_track_id,
     o.oom_adj_thread_name AS oom_adj_thread_name,
     o.oom_adj_reason AS oom_adj_reason,
     o.oom_adj_trigger AS oom_adj_trigger,
     m.anon_rss AS anon_rss,
     m.file_rss AS file_rss,
     m.shmem_rss AS shmem_rss,
     m.file_rss + m.anon_rss + coalesce(m.shmem_rss, 0) AS rss,
     m.swap AS swap,
     m.anon_rss + coalesce(m.swap, 0) AS anon_rss_and_swap,
     m.anon_rss + m.file_rss + coalesce(m.shmem_rss, 0) + coalesce(m.swap, 0) AS rss_and_swap;
