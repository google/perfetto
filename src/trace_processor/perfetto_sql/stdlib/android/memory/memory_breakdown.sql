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

INCLUDE PERFETTO MODULE android.oom_adjuster;

INCLUDE PERFETTO MODULE counters.intervals;

-- A table of process memory counter intervals, clipped to the process lifetime.
-- Provides a comprehensive view of memory usage, including raw values,
-- zygote-adjusted values, and a flag for tracks with a spike bigger than 100 MiB.
CREATE PERFETTO TABLE android_process_memory_intervals (
  -- The counter id
  id JOINID(counter.id),
  -- Timestamp of the memory counter sample.
  ts TIMESTAMP,
  -- Duration of the sample.
  dur DURATION,
  -- The unique process id.
  upid JOINID(process.id),
  -- The name of the memory counter track.
  memory_track_name STRING,
  -- The id of the memory counter track.
  track_id JOINID(track.id),
  -- The value of the memory counter.
  value LONG,
  -- The value of the memory counter, adjusted with the zygote's memory usage.
  zygote_adjusted_value LONG,
  -- Whether the track has a spike greater than 100MiB.
  -- This can happen because of rss_stat accounting issue: see b/418231246 for details.
  track_has_spike_gt_100mib BOOL
) AS
WITH
  -- Step 1: Prepare memory counter data.
  mem_tracks AS (
    SELECT
      id AS track_id,
      iif(name = 'Heap size (KB)', 'mem.heap', name) AS name,
      upid
    FROM process_counter_track
    WHERE
      name IN ('mem.rss.anon', 'mem.swap', 'mem.rss.file', 'mem.rss.shmem', 'Heap size (KB)', 'mem.dmabuf_rss', 'mem.locked', 'GPU Memory')
  ),
  mem_counters AS (
    SELECT
      c.id,
      c.track_id,
      c.ts,
      iif(t.name = 'mem.heap', cast_int!(c.value) * 1024, cast_int!(c.value)) AS value
    FROM counter AS c
    JOIN mem_tracks AS t
      ON c.track_id = t.track_id
  ),
  mem_intervals AS (
    SELECT
      ts,
      dur,
      id AS counter_id,
      track_id,
      value,
      delta_value
    FROM counter_leading_intervals!(mem_counters)
  ),
  -- Step 2: Identify tracks with large spikes (spikes > 100MiB).
  spikes AS (
    SELECT DISTINCT
      track_id
    FROM mem_intervals
    WHERE
      abs(delta_value) > 104857600
  ),
  -- Step 3: Join memory intervals with process lifetime and clip them.
  mem_intervals_clipped AS (
    SELECT
      max(ts, coalesce(p.start_ts, ts)) AS ts,
      min(i.ts + i.dur, coalesce(p.end_ts, i.ts + i.dur)) - max(i.ts, coalesce(p.start_ts, i.ts)) AS dur,
      p.upid,
      t.name AS memory_track_name,
      i.track_id,
      i.counter_id,
      i.value,
      NOT s.track_id IS NULL AS track_has_spike_gt_100mib
    FROM mem_intervals AS i
    JOIN mem_tracks AS t
      ON i.track_id = t.track_id
    JOIN process AS p
      USING (upid)
    LEFT JOIN spikes AS s
      USING (track_id)
    WHERE
      (
        min(i.ts + i.dur, coalesce(p.end_ts, i.ts + i.dur)) - max(i.ts, coalesce(p.start_ts, i.ts))
      ) > 0
  ),
  -- Step 4: Calculate zygote memory baseline.
  -- TODO: improve zygote process detection
  zygote_processes AS (
    SELECT
      upid
    FROM process
    WHERE
      name IN ('zygote', 'zygote64', 'webview_zygote')
  ),
  zygote_tracks AS (
    SELECT
      t.name AS memory_track_name,
      t.track_id
    FROM mem_tracks AS t
    JOIN zygote_processes AS z
      USING (upid)
    WHERE
      t.name IN ('mem.rss.anon', 'mem.swap', 'mem.rss.file', 'mem.heap')
  ),
  zygote_baseline AS (
    SELECT
      max(CASE WHEN memory_track_name = 'mem.rss.anon' THEN avg_val END) AS rss_anon_base,
      max(CASE WHEN memory_track_name = 'mem.swap' THEN avg_val END) AS swap_base,
      max(CASE WHEN memory_track_name = 'mem.rss.file' THEN avg_val END) AS rss_file_base,
      max(CASE WHEN memory_track_name = 'mem.heap' THEN avg_val END) AS heap_base
    FROM (
      SELECT
        z.memory_track_name,
        avg(cast_int!(c.value)) AS avg_val
      FROM mem_counters AS c
      JOIN zygote_tracks AS z
        USING (track_id)
      GROUP BY
        z.memory_track_name
    )
  ),
  -- Step 5: Join clipped intervals with zygote baseline.
  mem_intervals_with_zygote_baseline AS (
    SELECT
      c.*,
      CASE
        WHEN c.memory_track_name = 'mem.rss.anon'
        THEN zb.rss_anon_base
        WHEN c.memory_track_name = 'mem.swap'
        THEN zb.swap_base
        WHEN c.memory_track_name = 'mem.rss.file'
        THEN zb.rss_file_base
        WHEN c.memory_track_name = 'mem.heap'
        THEN zb.heap_base
        ELSE 0
      END AS zygote_baseline_value
    FROM mem_intervals_clipped AS c
    CROSS JOIN zygote_baseline AS zb
  )
-- Final Step: Compute the zygote-adjusted memory value.
SELECT
  d.counter_id AS id,
  d.ts,
  d.dur,
  d.upid,
  d.memory_track_name,
  d.track_id,
  d.value,
  CASE
    WHEN NOT p.upid IS NULL AND NOT p.name IN ('zygote', 'zygote64', 'webview_zygote')
    THEN max(0, cast_int!(d.value) - cast_int!(COALESCE(d.zygote_baseline_value, 0)))
    ELSE cast_int!(d.value)
  END AS zygote_adjusted_value,
  d.track_has_spike_gt_100mib
FROM mem_intervals_with_zygote_baseline AS d
LEFT JOIN process AS p
  USING (upid);

-- Create a table containing intervals of OOM adjustment scores.
-- This table will be used as the right side of a span join.
CREATE PERFETTO TABLE _memory_breakdown_oom_intervals_prepared AS
WITH
  process_mem_track_ids AS (
    SELECT
      track_id,
      upid
    FROM android_process_memory_intervals
    GROUP BY
      track_id,
      upid
  )
SELECT
  t.track_id,
  o.ts,
  o.dur,
  o.bucket
FROM android_oom_adj_intervals AS o
JOIN process_mem_track_ids AS t
  USING (upid)
WHERE
  o.dur > 0;

CREATE VIRTUAL TABLE _memory_breakdown_mem_oom_span_join USING SPAN_LEFT_JOIN (
  android_process_memory_intervals PARTITIONED track_id,
  _memory_breakdown_oom_intervals_prepared PARTITIONED track_id
);

-- Correlates memory counters with OOM adjustment scores.
--
-- This table joins memory counters with OOM adjustment scores, providing
-- insights into memory usage under system memory pressure.
CREATE PERFETTO TABLE android_process_memory_intervals_by_oom_bucket (
  -- Unique identifier
  id LONG,
  -- The start timestamp of the interval.
  ts TIMESTAMP,
  -- The duration of the interval.
  dur DURATION,
  -- The name of the process.
  process_name STRING,
  -- The unique process ID.
  upid JOINID(process.upid),
  -- The process ID.
  pid LONG,
  -- The OOM adjustment score bucket.
  bucket STRING,
  -- The name of the memory counter track.
  memory_track_name STRING,
  -- The id of the memory counter track.
  track_id JOINID(track.id),
  -- The counter id
  counter_id JOINID(counter.id),
  -- The memory counter value.
  value LONG,
  -- The zygote-adjusted memory value.
  zygote_adjusted_value LONG,
  -- Whether the track has a spike greater than 100MiB.
  -- This can happen because of rss_stat accounting issue: see b/418231246 for details.
  track_has_spike_gt_100mib BOOL
) AS
SELECT
  row_number() OVER () AS id,
  ts,
  dur,
  p.name AS process_name,
  upid,
  pid,
  coalesce(m.bucket, 'unknown') AS bucket,
  m.memory_track_name,
  m.track_id,
  m.id AS counter_id,
  m.value,
  m.zygote_adjusted_value,
  m.track_has_spike_gt_100mib
FROM _memory_breakdown_mem_oom_span_join AS m
LEFT JOIN process AS p
  USING (upid)
WHERE
  dur > 0;
