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
  -- We deny tracks that have large swings in value
  -- This can happen because of rss_stat accounting issue: see b/418231246 for details.
  mem_process_counter_tracks AS (
    SELECT
      id,
      name,
      upid
    FROM process_counter_track
    WHERE
      name IN ('mem.rss.anon', 'mem.swap', 'mem.rss.file')
  ),
  mem_counters AS (
    SELECT
      c.id,
      c.ts,
      c.track_id,
      c.value
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
  )
SELECT
  max(ts, coalesce(start_ts, ts)) AS ts,
  min(raw_end_ts, coalesce(end_ts, raw_end_ts)) - max(ts, coalesce(start_ts, ts)) AS dur,
  upid,
  track_name,
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
SELECT
  ts,
  dur,
  upid,
  bucket
FROM android_oom_adj_intervals
WHERE
  dur > 0;

-- Create a virtual table that joins memory counter intervals with OOM
-- adjustment score intervals.
CREATE VIRTUAL TABLE _memory_breakdown_mem_oom_span_join USING SPAN_LEFT_JOIN (
  _memory_breakdown_mem_intervals_raw PARTITIONED upid,
  _memory_breakdown_oom_intervals_prepared PARTITIONED upid
);

-- Create a table containing memory counter intervals with OOM buckets.
CREATE PERFETTO TABLE _memory_breakdown_mem_with_buckets AS
WITH
  -- Get the baseline values for RSS anon and swap from the zygote process.
  zygote_baseline AS (
    SELECT
      max(CASE WHEN track_name = 'mem.rss.anon' THEN avg_val END) AS rss_anon_base,
      max(CASE WHEN track_name = 'mem.swap' THEN avg_val END) AS swap_base,
      max(CASE WHEN track_name = 'mem.rss.file' THEN avg_val END) AS rss_file_base
    FROM (
      SELECT
        t.name AS track_name,
        avg(c.value) AS avg_val
      FROM counter AS c
      JOIN process_counter_track AS t
        ON c.track_id = t.id
      JOIN process AS p
        USING (upid)
      WHERE
        -- TODO: improve zygote process detection
        p.name IN ('zygote', 'zygote64', 'webview_zygote')
        AND t.name IN ('mem.rss.anon', 'mem.swap', 'mem.rss.file')
      GROUP BY
        t.name
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
  app.name AS process_name,
  upid,
  pid,
  coalesce(bucket, 'unknown') AS bucket,
  CASE
    WHEN app.upid IS NOT NULL
    THEN max(0, cast_int!(value) - cast_int!(IFNULL(zygote_baseline_value, 0)))
    ELSE cast_int!(value)
  END AS zygote_adjusted_value
FROM mem_with_zygote_baseline
LEFT JOIN process AS app
  USING (upid)
WHERE
  dur > 0;
