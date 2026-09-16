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

INCLUDE PERFETTO MODULE android.cpu.cluster_type;

INCLUDE PERFETTO MODULE intervals.intersect;

-- CPU utilization broken down by cluster over a given interval.
--
-- Returns one row per cluster. Clusters which ran nothing during the interval
-- are still reported, with zero active time and zero utilization.
--
-- Durations are measured on the monotonic clock, so time spent with the device
-- suspended is excluded.
CREATE PERFETTO FUNCTION android_cpu_cluster_utilization_in_interval(
  -- Start of the interval.
  ts TIMESTAMP,
  -- Duration of the interval.
  dur LONG
)
RETURNS TABLE(
  -- Cluster name ('little', 'medium', 'big', 'unknown').
  cluster_type STRING,
  -- Number of cores in the cluster.
  core_count LONG,
  -- Active core-time in the interval, summed across the cluster's cores.
  active_dur DURATION,
  -- Fraction of the cluster's available core-time spent running, in [0.0, 1.0]:
  -- active_dur / (awake_dur * core_count). 1.0 means every core in the cluster
  -- was busy for the whole interval.
  utilization DOUBLE
)
AS
WITH
  -- Awake duration of the interval, excluding time spent suspended.
  awake AS (SELECT to_monotonic($ts + $dur) - to_monotonic($ts) AS dur_ns),
  cluster_cores AS (
    SELECT
      coalesce(c.cluster_type, 'unknown') AS cluster_type,
      count(DISTINCT cpu.ucpu) AS core_count
    FROM cpu
    LEFT JOIN android_cpu_cluster_mapping AS c USING (ucpu)
    GROUP BY
      1
  ),
  -- Sched slices of running tasks. thread(s) with is_idle = 1 are the swapper
  -- threads / idle tasks.
  active_sched AS (
    SELECT id, ts, dur, ucpu
    FROM sched
    WHERE
      NOT (utid IN (SELECT utid FROM thread WHERE is_idle))
      AND dur > 0
  ),
  cluster_active AS (
    SELECT
      coalesce(c.cluster_type, 'unknown') AS cluster_type,
      sum(to_monotonic(ii.ts + ii.dur) - to_monotonic(ii.ts)) AS active_dur_ns
    FROM _interval_intersect_single!($ts, $dur, active_sched) AS ii
    JOIN active_sched AS s
      ON s.id = ii.id
    LEFT JOIN android_cpu_cluster_mapping AS c USING (ucpu)
    GROUP BY
      1
  )
SELECT
  cc.cluster_type,
  cc.core_count,
  coalesce(ca.active_dur_ns, 0) AS active_dur,
  iif(
    awake.dur_ns > 0
    AND cc.core_count > 0,
    coalesce(ca.active_dur_ns, 0) * 1.0 / (awake.dur_ns * cc.core_count),
    0.0
  ) AS utilization
FROM cluster_cores AS cc
CROSS JOIN awake
LEFT JOIN cluster_active AS ca USING (cluster_type);
