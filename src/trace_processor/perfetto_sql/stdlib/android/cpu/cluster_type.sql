--
-- Copyright 2024 The Android Open Source Project
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

-- Uses cluster_id which has been calculated using the cpu_capacity in order
-- to determine the cluster type for cpus with 2, 3 or 4 clusters
-- indicating whether they are "little", "medium" or "big".

-- Stores the mapping of a cpu to its cluster type - e.g. little, medium, big.
-- This cluster type is determined by initially using cpu_capacity from sysfs
-- and grouping clusters with identical capacities, ordered by size.
-- In the case that capacities are not present, max frequency is used instead.
-- If nothing is avaiable, NULL is returned.
CREATE PERFETTO PIPELINE android_cpu_cluster_mapping(
  -- Alias of `cpu.ucpu`.
  ucpu LONG,
  -- Alias of `cpu.cpu`.
  cpu LONG,
  -- The cluster type of the CPU.
  cluster_type STRING
) MATERIALIZED AS
SUBPIPELINE cores AS (
  FROM (
    VALUES
      (0, 'little', 2),
      (1, 'big', 2),
      (0, 'little', 3),
      (1, 'medium', 3),
      (2, 'big', 3),
      (0, 'little', 4),
      (1, 'medium', 4),
      (2, 'medium', 4),
      (3, 'big', 4)
  ) AS data(cluster_id, cluster_type, cluster_count)
)
FROM cpu
|> LEFT JOIN cores
     ON cores.cluster_id = cpu.cluster_id
     AND cores.cluster_count = (SELECT count(DISTINCT cluster_id) FROM cpu)
|> SELECT ucpu, cpu, cores.cluster_type AS cluster_type;

-- The count of active CPUs with a given cluster type over time.
CREATE PERFETTO MACRO _active_cpu_count_for_cluster_type(
  -- Type of the CPU cluster as reported by android_cpu_cluster_mapping. Usually 'little', 'medium' or 'big'.
  cluster_type Expr
)
-- Returns a pipeline of (ts TIMESTAMP, active_cpu_count LONG), where ts is the
-- timestamp when the number of active CPUs changed and active_cpu_count covers
-- the range from this timestamp to the next row's timestamp.
RETURNS Pipeline
AS (
  -- Filter sched events corresponding to running tasks on the relevant clusters.
  -- thread(s) with is_idle = 1 are the swapper threads / idle tasks.
  FROM sched
  |> WHERE ucpu IN (
       SELECT ucpu
       FROM android_cpu_cluster_mapping
       WHERE cluster_type = $cluster_type
     )
     AND NOT (utid IN (SELECT utid FROM thread WHERE is_idle))
  -- Resolve overlap into disjoint segments carrying the live task count, then
  -- fill the uncovered gaps with zero-count fillers spanning the trace.
  |> INTERVAL FLATTEN AGGREGATE COUNT(*) AS active_cpu_count
  |> INTERVAL FILL WITHIN trace_bounds
  |> SELECT ts, coalesce(active_cpu_count, 0) AS active_cpu_count
  |> ORDER BY ts
);
