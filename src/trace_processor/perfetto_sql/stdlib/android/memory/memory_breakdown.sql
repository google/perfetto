--
-- Copyright 2026 The Android Open Source Project
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

-- NOTE (psqlnext): `counters.intervals` is DELETED — `counter_leading_intervals!`
-- is `INTERVALS FROM EVENTS` (+`MERGE CONSECUTIVE BY value`); the partitioned
-- `SPAN_LEFT_JOIN` with the OOM intervals is `INTERVAL SPLIT ... PER track_id`.

INCLUDE PERFETTO MODULE android.oom_adjuster;

-- A table of process memory counter intervals, clipped to the process lifetime.
-- Provides a comprehensive view of memory usage, including raw values and
-- zygote-adjusted values.
--
-- NOTE: For 'mem.rss.anon', 'mem.swap', 'mem.rss.file', and 'mem.heap' tracks, we
-- subtract the average memory usage of the zygote that forked the process (its
-- parent). This provides a better estimate of the child process's unique memory
-- usage by accounting for the baseline memory inherited from its zygote. This is
-- a no-op for a process whose forking zygote has no memory samples in the trace
-- (e.g. traces with only ftrace rss_stat, where idle zygotes are not sampled;
-- process_stats polling captures them).
--
-- NOTE: Some tracks may have a spike greater than 100MiB. This can be legitimate or
-- an accounting issue: see b/418231246 for more details.
CREATE PERFETTO PIPELINE android_process_memory_intervals(
  -- The id of the memory counter value
  id JOINID(counter.id),
  -- Timestamp of the memory counter change.
  ts TIMESTAMP,
  -- How long this memory track had this value.
  dur DURATION,
  -- The name of the process whose memory is being measured.
  process_name STRING,
  -- The unique id of the process whose memory is being measured.
  upid JOINID(process.upid),
  -- The id of the process whose memory is being measured.
  pid JOINID(process.pid),
  -- The name of the memory counter track (e.g. 'mem.rss.anon').
  memory_track_name STRING,
  -- The id of the memory counter track.
  track_id JOINID(track.id),
  -- The value of the memory counter in bytes.
  value LONG,
  -- The value of the memory counter in bytes, adjusted with the zygote's memory usage.
  zygote_adjusted_value LONG,
  -- Whether the track has a spike greater than 100MiB.
  track_has_spike_gt_100mib BOOL
)
MATERIALIZED AS
-- Step 1: Prepare memory counter data.
SUBPIPELINE mem_tracks AS (
  FROM process_counter_track
  |> WHERE name IN (
       'mem.rss.anon',
       'mem.swap',
       'mem.rss.file',
       'mem.rss.shmem',
       'Heap size (KB)',
       'mem.dmabuf_rss',
       'mem.locked',
       'GPU Memory'
     )
  |> SELECT
       id AS track_id,
       iif(name = 'Heap size (KB)', 'mem.heap', name) AS name,
       upid
)
SUBPIPELINE mem_counters AS (
  FROM counter AS c
  |> JOIN mem_tracks AS t
     ON c.track_id = t.track_id
  |> SELECT
       c.id,
       c.track_id,
       c.ts,
       iif(t.name = 'mem.heap', cast_int!(c.value) * 1024, cast_int!(c.value)) AS value
)
-- Counter samples become leading intervals; equal-valued runs coalesce and
-- `delta_value` is recovered from the previous value in lane order.
SUBPIPELINE mem_intervals AS (
  INTERVALS FROM EVENTS mem_counters PER track_id CLOSING LAST AT (trace_end())
  |> INTERVAL MERGE CONSECUTIVE BY value
  |> SELECT
       ts,
       dur,
       id AS counter_id,
       track_id,
       value,
       value - LAG(value) OVER (PARTITION BY track_id ORDER BY ts) AS delta_value
)
-- Step 2: Identify tracks with large spikes (spikes > 100MiB).
SUBPIPELINE spikes AS (
  FROM mem_intervals
  |> WHERE abs(delta_value) > 104857600
  |> SELECT DISTINCT track_id
)
-- Step 4: Per-zygote memory baseline and the process->forking-zygote map.
-- A process inherits memory (via copy-on-write) only from the specific zygote
-- that forked it, so the baseline is kept per-zygote and attributed by parent.
-- Only the primary zygotes; secondary zygotes (webview_zygote, per-app
-- '<package>_zygote') are intentionally treated as ordinary processes.
SUBPIPELINE zygote_processes AS (
  FROM process |> WHERE name IN ('zygote', 'zygote64') |> SELECT upid
)
SUBPIPELINE zygote_baseline AS (
  FROM mem_counters AS c
  |> JOIN mem_tracks AS t USING (track_id)
  |> JOIN zygote_processes AS z
     ON t.upid = z.upid
  |> WHERE t.name IN ('mem.rss.anon', 'mem.swap', 'mem.rss.file', 'mem.heap')
  |> AGGREGATE
       avg(cast_int!(c.value)) AS baseline_value
     GROUP BY t.upid AS zygote_upid, t.name AS memory_track_name
)
-- Map each process to its forking zygote (its parent), when the parent is a
-- zygote. Processes not forked from a zygote match nothing here.
SUBPIPELINE process_zygote_parent AS (
  FROM process AS p
  |> JOIN zygote_processes AS z
     ON p.parent_upid = z.upid
  |> SELECT p.upid, p.parent_upid AS zygote_upid
)
-- Step 3: Join memory intervals with process lifetime and clip them.
FROM mem_intervals AS i
|> JOIN mem_tracks AS t
   ON i.track_id = t.track_id
|> JOIN process AS p USING (upid)
|> LEFT JOIN spikes AS s USING (track_id)
|> WHERE
     (min(i.ts + i.dur, coalesce(p.end_ts, i.ts + i.dur))
     - max(i.ts, coalesce(p.start_ts, i.ts)))
     > 0
|> SELECT
     max(i.ts, coalesce(p.start_ts, i.ts)) AS ts,
     min(i.ts + i.dur, coalesce(p.end_ts, i.ts + i.dur))
     - max(i.ts, coalesce(p.start_ts, i.ts)) AS dur,
     p.upid,
     t.name AS memory_track_name,
     i.track_id,
     i.counter_id,
     i.value,
     NOT (s.track_id IS NULL) AS track_has_spike_gt_100mib
-- Step 5: Attach the baseline of the process's parent zygote for that track
-- (0 when not forked from a zygote, or not an adjustable metric).
|> LEFT JOIN process_zygote_parent AS pzp
   ON upid = pzp.upid
|> LEFT JOIN zygote_baseline AS zb
   ON zb.zygote_upid = pzp.zygote_upid
   AND zb.memory_track_name = memory_track_name
|> LEFT JOIN process AS p2 USING (upid)
-- Final Step: Compute the zygote-adjusted memory value.
|> SELECT
     counter_id AS id,
     ts,
     dur,
     p2.name AS process_name,
     upid,
     p2.pid,
     memory_track_name,
     track_id,
     value,
     CASE
       WHEN NOT (p2.upid IS NULL)
       AND upid NOT IN (SELECT upid FROM zygote_processes) THEN max(
         0,
         cast_int!(value) - cast_int!(COALESCE(zb.baseline_value, 0))
       )
       ELSE cast_int!(value)
     END AS zygote_adjusted_value,
     track_has_spike_gt_100mib;

-- Correlates memory counters with OOM adjustment scores.
--
-- This table joins memory counters with OOM adjustment scores, providing
-- insights into memory usage under system memory pressure.
--
-- NOTE: For 'mem.rss.anon', 'mem.swap', 'mem.rss.file', and 'mem.heap' tracks, we
-- subtract the average memory usage of the zygote that forked the process (its
-- parent). This provides a better estimate of the child process's unique memory
-- usage by accounting for the baseline memory inherited from its zygote. This is
-- a no-op for a process whose forking zygote has no memory samples in the trace
-- (e.g. traces with only ftrace rss_stat, where idle zygotes are not sampled;
-- process_stats polling captures them).
--
-- NOTE: Some tracks may have a spike greater than 100MiB. This can be legitimate or
-- an accounting issue: see b/418231246 for more details.
CREATE PERFETTO PIPELINE android_process_memory_intervals_by_oom_bucket(
  -- Id.
  id LONG,
  -- The start timestamp of the interval.
  ts TIMESTAMP,
  -- How long this memory track had this value.
  dur DURATION,
  -- The name of the process whose memory is being measured.
  process_name STRING,
  -- The unique id of the process whose memory is being measured.
  upid JOINID(process.upid),
  -- The id of the process whose memory is being measured.
  pid JOINID(process.pid),
  -- The OutOfMemory (OOM) adjustment score bucket (e.g. 'cached', 'background'). Defaults to 'unknown'
  -- if no OOM score is available for the interval.
  bucket STRING,
  -- The name of the memory counter track (e.g. 'mem.rss.anon').
  memory_track_name STRING,
  -- The id of the memory counter track.
  track_id JOINID(track.id),
  -- The id of the memory counter value
  counter_id JOINID(counter.id),
  -- The value of the memory counter in bytes.
  value LONG,
  -- The value of the memory counter in bytes, adjusted with the zygote's memory usage.
  zygote_adjusted_value LONG,
  -- Whether the track has a spike greater than 100MiB.
  track_has_spike_gt_100mib BOOL
)
MATERIALIZED AS
-- OOM intervals to overlay, keyed by the memory track of the same process.
SUBPIPELINE oom_intervals AS (
  SUBPIPELINE process_mem_track_ids AS (
    FROM android_process_memory_intervals
    |> AGGREGATE GROUP BY track_id, upid
  )
  FROM android_oom_adj_intervals AS o
  |> JOIN process_mem_track_ids AS t USING (upid)
  |> WHERE o.dur > 0
  |> SELECT t.track_id, o.ts, o.dur, o.bucket
)
-- Split memory intervals at OOM-bucket boundaries (left side preserved), so each
-- fragment carries the bucket that was in effect (null -> 'unknown').
FROM android_process_memory_intervals AS m
|> INTERVAL SPLIT oom_intervals AS o PER track_id
|> WHERE dur > 0
|> SELECT
     row_number() OVER () AS id,
     m.ts,
     m.dur,
     m.process_name,
     m.upid,
     m.pid,
     coalesce(o.bucket, 'unknown') AS bucket,
     m.memory_track_name,
     m.track_id,
     m.id AS counter_id,
     m.value,
     m.zygote_adjusted_value,
     m.track_has_spike_gt_100mib;
