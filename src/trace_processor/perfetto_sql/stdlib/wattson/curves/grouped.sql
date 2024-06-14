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

INCLUDE PERFETTO MODULE wattson.curves.ungrouped;

-- Wattson's estimated usage of the system, split out into cpu cluster based on
-- the natural grouping of the hardware.
CREATE PERFETTO TABLE wattson_estimate_per_component(
  -- Starting timestamp of the slice
  ts LONG,
  -- Duration of the slice
  dur INT,
  -- Total L3 estimated usage in mW during this slice
  l3 FLOAT,
  -- Total little CPU estimated usage in mW during this slice
  little_cpus FLOAT,
  -- Total mid CPU cluster estimated usage in mW during this slice
  mid_cpus FLOAT,
  -- Total big CPU cluster estimated usage in mW during this slice
  big_cpus FLOAT
)
AS
SELECT
  ts,
  dur,
  IFNULL(l3_hit_value, 0.0) + IFNULL(l3_miss_value, 0.0) as l3,
  IFNULL(cpu0_curve, 0.0) + IFNULL(cpu1_curve, 0.0) + IFNULL(cpu2_curve, 0.0) +
    IFNULL(cpu3_curve, 0.0) + static_curve as little_cpus,
  cpu4_curve + cpu5_curve as mid_cpus,
  cpu6_curve + cpu7_curve as big_cpus
FROM _system_state_curves;

-- Gives total contribution of each HW component for the entire trace, bringing
-- the output of the table to parity with the Python version of Wattson
CREATE PERFETTO TABLE _wattson_entire_trace
AS
WITH _individual_totals AS (
  SELECT
    -- LUT for l3 is scaled by 10^6 to save resolution, so do the inversion
    -- scaling by 10^6 after the summation to minimize losing resolution
    SUM(l3) / 1000000 as total_l3,
    SUM(dur * little_cpus) / 1000000000 as total_little_cpus,
    SUM(dur * mid_cpus) / 1000000000 as total_mid_cpus,
    SUM(dur * big_cpus) / 1000000000 as total_big_cpus
  FROM wattson_estimate_per_component
  )
SELECT
  ROUND(total_l3, 2) as total_l3,
  ROUND(total_little_cpus, 2) as total_little_cpus,
  ROUND(total_mid_cpus, 2) as total_mid_cpus,
  ROUND(total_big_cpus, 2) as total_big_cpus,
  ROUND(total_l3 + total_little_cpus + total_mid_cpus + total_big_cpus, 2) as total
FROM _individual_totals;

