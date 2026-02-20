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

-- Create a table containing intervals of memory counters values, adjusted to process lifetime.
CREATE PERFETTO TABLE _memory_breakdown_mem_intervals_raw AS
WITH
  sched_bounds AS (
    SELECT
      min(ts) AS min_ts,
      max(ts + dur) AS max_ts
    FROM sched
  ),
  trace_limits AS (
    SELECT
      coalesce(sb.min_ts, trace_start()) AS start_ts,
      coalesce(sb.max_ts, trace_end()) AS end_ts
    FROM sched_bounds AS sb
  ),
  mem_process_counter_tracks AS (
    SELECT
      id,
      iif(name = 'Heap size (KB)', 'mem.heap', name) AS name,
      upid
    FROM process_counter_track
    WHERE
      name IN ('mem.rss.anon', 'mem.swap', 'mem.rss.file', 'mem.rss.shmem', 'Heap size (KB)', 'mem.dmabuf_rss', 'mem.locked', 'GPU Memory')
  ),
  mem_counters AS (
    SELECT
      c.id,
      c.ts,
      c.track_id,
      iif(name = 'mem.heap', cast_int!(c.value) * 1024, cast_int!(c.value)) AS value
    FROM counter AS c
    JOIN mem_process_counter_tracks AS t
      ON c.track_id = t.id
  ),
  mem_intervals AS (
    SELECT
      ts,
      dur,
      track_id,
      value,
      delta_value
    FROM counter_leading_intervals!(mem_counters)
  ),
  -- We deny tracks that have large swings in value
  -- This can happen because of rss_stat accounting issue: see b/418231246 for details.
  denied_tracks AS (
    SELECT DISTINCT
      track_id
    FROM mem_intervals
    WHERE
      -- Filter out changes larger than 100 MiB
      abs(delta_value) > 104857600
  ),
  -- Get all memory counter values for all processes, and clip them to process lifetime.
  mem_intervals_with_process_lifetime AS (
    SELECT
      i.ts,
      i.ts + i.dur AS raw_end_ts,
      p.upid,
      p.start_ts,
      p.end_ts,
      t.name AS track_name,
      i.track_id,
      i.value
    FROM mem_intervals AS i
    JOIN mem_process_counter_tracks AS t
      ON i.track_id = t.id
    JOIN process AS p
      USING (upid)
    WHERE
      NOT i.track_id IN (
        SELECT
          track_id
        FROM denied_tracks
      )
      AND i.ts BETWEEN (
        SELECT
          start_ts
        FROM trace_limits
      ) AND (
        SELECT
          end_ts
        FROM trace_limits
      )
  )
SELECT
  max(ts, coalesce(start_ts, ts)) AS ts,
  min(raw_end_ts, coalesce(end_ts, raw_end_ts)) - max(ts, coalesce(start_ts, ts)) AS dur,
  upid,
  track_name,
  track_id,
  value
FROM mem_intervals_with_process_lifetime
-- Only keep rows where the clipping resulted in a positive duration.
WHERE
  (
    min(raw_end_ts, coalesce(end_ts, raw_end_ts)) - max(ts, coalesce(start_ts, ts))
  ) > 0;

-- Create a table containing intervals of OOM adjustment scores.
-- This table will be used as the right side of a span join.
CREATE PERFETTO TABLE _memory_breakdown_oom_intervals_prepared AS
WITH
  process_mem_track_ids AS (
    SELECT
      track_id,
      upid
    FROM _memory_breakdown_mem_intervals_raw
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
  _memory_breakdown_mem_intervals_raw PARTITIONED track_id,
  _memory_breakdown_oom_intervals_prepared PARTITIONED track_id
);

-- Create a table containing memory counter intervals with OOM buckets.
CREATE PERFETTO TABLE _memory_breakdown_mem_with_buckets AS
WITH
  -- Get the baseline values for RSS anon and swap from the zygote process.
  zygote_upid AS (
    SELECT
      upid
    FROM process
    -- TODO: improve zygote process detection
    WHERE
      name IN ('zygote', 'zygote64', 'webview_zygote')
  ),
  zygote_tracks AS (
    SELECT
      iif(t.name = 'Heap size (KB)', 'mem.heap', t.name) AS track_name,
      t.id AS track_id
    FROM process_counter_track AS t
    JOIN zygote_upid AS z
      USING (upid)
    WHERE
      t.name IN ('mem.rss.anon', 'mem.swap', 'mem.rss.file', 'Heap size (KB)')
  ),
  zygote_baseline AS (
    SELECT
      max(CASE WHEN track_name = 'mem.rss.anon' THEN avg_val END) AS rss_anon_base,
      max(CASE WHEN track_name = 'mem.swap' THEN avg_val END) AS swap_base,
      max(CASE WHEN track_name = 'mem.rss.file' THEN avg_val END) AS rss_file_base,
      max(CASE WHEN track_name = 'mem.heap' THEN avg_val END) AS heap_base
    FROM (
      SELECT
        z.track_name,
        avg(iif(z.track_name = 'mem.heap', cast_int!(c.value) * 1024, cast_int!(c.value))) AS avg_val
      FROM counter AS c
      JOIN zygote_tracks AS z
        USING (track_id)
      GROUP BY
        z.track_name
    )
  ),
  mem_with_zygote_baseline AS (
    SELECT
      s.*,
      CASE
        WHEN track_name = 'mem.rss.anon'
        THEN b.rss_anon_base
        WHEN track_name = 'mem.swap'
        THEN b.swap_base
        WHEN track_name = 'mem.rss.file'
        THEN b.rss_file_base
        WHEN track_name = 'mem.heap'
        THEN b.heap_base
        ELSE 0
      END AS zygote_baseline_value
    FROM _memory_breakdown_mem_oom_span_join AS s
    CROSS JOIN zygote_baseline AS b
  )
SELECT
  row_number() OVER () AS id,
  ts,
  dur,
  track_name,
  p.name AS process_name,
  upid,
  pid,
  coalesce(bucket, 'unknown') AS bucket,
  CASE
    WHEN NOT p.upid IS NULL AND NOT p.name IN ('zygote', 'zygote64', 'webview_zygote')
    THEN max(0, cast_int!(value) - cast_int!(IFNULL(zygote_baseline_value, 0)))
    ELSE cast_int!(value)
  END AS zygote_adjusted_value
FROM mem_with_zygote_baseline
LEFT JOIN process AS p
  USING (upid)
WHERE
  dur > 0;
