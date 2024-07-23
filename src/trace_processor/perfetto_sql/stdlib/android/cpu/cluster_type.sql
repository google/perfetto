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

CREATE PERFETTO TABLE _cores AS
WITH data(cluster_id, cluster_type, cluster_count) AS (
  VALUES
    (0, 'little', 2), (1, 'big', 2),
    (0, 'little', 3), (1, 'medium', 3), (2, 'big', 3),
    (0, 'little', 4), (1, 'medium', 4), (2, 'medium', 4), (3, 'big', 4)
)
SELECT * FROM data;

-- Stores the mapping of a cpu to its cluster type - e.g. little, medium, big.
-- This cluster type is determined by initially using cpu_capacity from sysfs
-- and grouping clusters with identical capacities, ordered by size.
-- In the case that capacities are not present, max frequency is used instead.
-- If nothing is avaiable, NULL is returned.
CREATE PERFETTO TABLE android_cpu_cluster_mapping (
  -- Alias of `cpu.ucpu`.
  ucpu INT,
  -- Alias of `cpu.cpu`.
  cpu INT,
  -- The cluster type of the CPU.
  cluster_type STRING
) AS
SELECT
  ucpu,
  cpu,
  _cores.cluster_type AS cluster_type
FROM
  cpu
LEFT JOIN _cores ON _cores.cluster_id = cpu.cluster_id
AND _cores.cluster_count = (SELECT COUNT(DISTINCT cluster_id)FROM cpu)