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

INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_class_aggregation;

-- One row per Java heap dump, with its heap totals.
CREATE PERFETTO TABLE memscope_java_dump(
  -- Process id.
  upid JOINID(process.id),
  -- Timestamp of the dump.
  ts TIMESTAMP,
  -- Id of the heap_graph row of the dump.
  event_id LONG,
  -- Bytes of all objects.
  total_heap_size LONG,
  -- Count of all objects.
  total_obj_count LONG,
  -- Bytes of reachable objects.
  reachable_heap_size LONG,
  -- Native bytes registered against reachable objects.
  reachable_native_size LONG,
  -- Count of reachable objects.
  reachable_obj_count LONG
)
AS
SELECT
  s.upid,
  s.graph_sample_ts AS ts,
  g.id AS event_id,
  s.total_heap_size,
  s.total_obj_count,
  s.reachable_heap_size,
  s.reachable_native_alloc_registry_size AS reachable_native_size,
  s.reachable_obj_count
FROM android_heap_graph_stats AS s
LEFT JOIN heap_graph AS g ON g.upid = s.upid AND g.ts = s.graph_sample_ts
ORDER BY
  s.upid,
  s.graph_sample_ts;

-- The top classes of every Java heap dump, by retained size, shallow size
-- and object count: any class in the top 8 of one of the three orderings.
CREATE PERFETTO TABLE memscope_java_top_class(
  -- Process id.
  upid JOINID(process.id),
  -- Timestamp of the dump.
  ts TIMESTAMP,
  -- Class name.
  type_name STRING,
  -- Count of reachable instances.
  reachable_obj_count LONG,
  -- Bytes of reachable instances.
  reachable_size_bytes LONG,
  -- Native bytes of reachable instances.
  reachable_native_size_bytes LONG,
  -- Bytes retained by instances.
  dominated_size_bytes LONG,
  -- Native bytes retained by instances.
  dominated_native_size_bytes LONG,
  -- Rank by retained (java + native) size.
  rn_retained LONG,
  -- Rank by shallow reachable size.
  rn_shallow LONG,
  -- Rank by reachable instance count.
  rn_count LONG
)
AS
WITH
  ranked AS (
    SELECT
      a.*,
      row_number() OVER (
        PARTITION BY
          a.upid,
          a.graph_sample_ts
        ORDER BY a.dominated_size_bytes + a.dominated_native_size_bytes DESC
      ) AS rn_retained,
      row_number() OVER (
        PARTITION BY
          a.upid,
          a.graph_sample_ts
        ORDER BY a.reachable_size_bytes DESC
      ) AS rn_shallow,
      row_number() OVER (
        PARTITION BY
          a.upid,
          a.graph_sample_ts
        ORDER BY a.reachable_obj_count DESC
      ) AS rn_count
    FROM android_heap_graph_class_aggregation AS a
  )
SELECT
  upid,
  graph_sample_ts AS ts,
  ifnull(type_name, '[unknown]') AS type_name,
  reachable_obj_count,
  reachable_size_bytes,
  reachable_native_size_bytes,
  dominated_size_bytes,
  dominated_native_size_bytes,
  rn_retained,
  rn_shallow,
  rn_count
FROM ranked
WHERE
  rn_retained <= 8
  OR rn_shallow <= 8
  OR rn_count <= 8;

-- Growth of each Java class between the first and last heap dump of a
-- process. Only processes with at least two dumps have rows.
CREATE PERFETTO TABLE memscope_java_class_growth(
  -- Process id.
  upid JOINID(process.id),
  -- Class name.
  type_name STRING,
  -- Timestamp of the first dump.
  first_ts TIMESTAMP,
  -- Timestamp of the last dump.
  last_ts TIMESTAMP,
  -- Reachable bytes at the first dump.
  first_size_bytes LONG,
  -- Reachable bytes at the last dump.
  last_size_bytes LONG,
  -- last_size_bytes - first_size_bytes.
  delta_size_bytes LONG,
  -- Reachable instances at the first dump.
  first_obj_count LONG,
  -- Reachable instances at the last dump.
  last_obj_count LONG,
  -- last_obj_count - first_obj_count.
  delta_obj_count LONG
)
AS
WITH
  bounds AS (
    SELECT upid, min(ts) AS first_ts, max(ts) AS last_ts
    FROM memscope_java_dump
    GROUP BY
      upid
    HAVING
      count(*) > 1
  ),
  per_class AS (
    SELECT
      a.upid,
      ifnull(a.type_name, '[unknown]') AS type_name,
      sum(iif(a.graph_sample_ts = b.first_ts, a.reachable_size_bytes, 0)) AS first_size_bytes,
      sum(iif(a.graph_sample_ts = b.last_ts, a.reachable_size_bytes, 0)) AS last_size_bytes,
      sum(iif(a.graph_sample_ts = b.first_ts, a.reachable_obj_count, 0)) AS first_obj_count,
      sum(iif(a.graph_sample_ts = b.last_ts, a.reachable_obj_count, 0)) AS last_obj_count
    FROM android_heap_graph_class_aggregation AS a
    JOIN bounds AS b USING (upid)
    WHERE
      a.graph_sample_ts IN (b.first_ts, b.last_ts)
    GROUP BY
      a.upid,
      type_name
  )
SELECT
  p.upid,
  p.type_name,
  b.first_ts,
  b.last_ts,
  p.first_size_bytes,
  p.last_size_bytes,
  p.last_size_bytes - p.first_size_bytes AS delta_size_bytes,
  p.first_obj_count,
  p.last_obj_count,
  p.last_obj_count - p.first_obj_count AS delta_obj_count
FROM per_class AS p
JOIN bounds AS b USING (upid)
ORDER BY
  p.upid,
  delta_size_bytes DESC;
