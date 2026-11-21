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
INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_stats;

INCLUDE PERFETTO MODULE memscope.smaps;

-- Uptime and OOM score of each process at its last Java heap dump.
CREATE PERFETTO TABLE memscope_process_last_heap_dump(
  -- Process id.
  upid JOINID(process.id),
  -- Timestamp of the last heap dump.
  ts TIMESTAMP,
  -- Process uptime at the dump, in ns.
  uptime DURATION,
  -- oom_score_adj at the dump.
  oom_score_adj LONG
)
AS
SELECT upid, ts, uptime, oom_score_adj
FROM (
  SELECT
    upid,
    graph_sample_ts AS ts,
    process_uptime AS uptime,
    oom_score_adj,
    row_number() OVER (PARTITION BY upid ORDER BY graph_sample_ts DESC) AS rn
  FROM android_heap_graph_stats
)
WHERE
  rn = 1;

-- RSS counter maxima of each process over the whole trace.
CREATE PERFETTO TABLE memscope_process_rss_counters(
  -- Process id.
  upid JOINID(process.id),
  -- Max of the mem.rss counter.
  rss_max_bytes LONG,
  -- Max of the mem.rss.watermark counter.
  rss_watermark_bytes LONG,
  -- How far the watermark overshoots the best sampled RSS, clamped at 0.
  -- NULL unless both counters exist.
  rss_spike_bytes LONG
)
AS
WITH
  m AS (
    SELECT
      t.upid,
      max(iif(t.name = 'mem.rss', c.value, NULL)) AS rss_max,
      max(iif(t.name = 'mem.rss.watermark', c.value, NULL)) AS rss_watermark
    FROM counter AS c
    JOIN process_counter_track AS t ON c.track_id = t.id
    WHERE
      t.name IN ('mem.rss', 'mem.rss.watermark')
    GROUP BY
      t.upid
  )
SELECT
  upid,
  cast_int!(rss_max) AS rss_max_bytes,
  cast_int!(rss_watermark) AS rss_watermark_bytes,
  iif(
    rss_max IS NOT NULL
    AND rss_watermark IS NOT NULL,
    cast_int!(max(0, rss_watermark - rss_max)),
    NULL
  ) AS rss_spike_bytes
FROM m;

-- Anon + swap memory of each process across its smaps snapshots: the peak,
-- and the change from the first to the last snapshot.
CREATE PERFETTO TABLE memscope_process_memory_trend(
  -- Process id.
  upid JOINID(process.id),
  -- Number of smaps snapshots.
  snapshot_count LONG,
  -- Largest anon + swap over all snapshots.
  peak_bytes LONG,
  -- Timestamp of the first snapshot.
  first_ts TIMESTAMP,
  -- Timestamp of the last snapshot.
  last_ts TIMESTAMP,
  -- Anon + swap at the first snapshot.
  first_bytes LONG,
  -- Anon + swap at the last snapshot.
  last_bytes LONG,
  -- last_bytes - first_bytes. NULL with fewer than two snapshots.
  delta_bytes LONG,
  -- delta_bytes extrapolated to one hour. NULL with fewer than two snapshots.
  trend_bytes_per_hour DOUBLE
)
AS
WITH
  s AS (
    SELECT
      upid,
      ts,
      anon_swap_bytes,
      row_number() OVER (PARTITION BY upid ORDER BY ts ASC) AS rn_first,
      row_number() OVER (PARTITION BY upid ORDER BY ts DESC) AS rn_last
    FROM memscope_smaps_snapshot
  ),
  agg AS (
    SELECT
      upid,
      count(*) AS snapshot_count,
      max(anon_swap_bytes) AS peak_bytes,
      max(iif(rn_first = 1, ts, NULL)) AS first_ts,
      max(iif(rn_last = 1, ts, NULL)) AS last_ts,
      max(iif(rn_first = 1, anon_swap_bytes, NULL)) AS first_bytes,
      max(iif(rn_last = 1, anon_swap_bytes, NULL)) AS last_bytes
    FROM s
    GROUP BY
      upid
  )
SELECT
  upid,
  snapshot_count,
  peak_bytes,
  first_ts,
  last_ts,
  first_bytes,
  last_bytes,
  iif(snapshot_count > 1, last_bytes - first_bytes, NULL) AS delta_bytes,
  iif(
    snapshot_count > 1
    AND last_ts > first_ts,
    (last_bytes - first_bytes) * 3600e9 / (last_ts - first_ts),
    NULL
  ) AS trend_bytes_per_hour
FROM agg;
