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

INCLUDE PERFETTO MODULE wattson.curves.device_cpu_1d;

INCLUDE PERFETTO MODULE wattson.curves.device_cpu_2d;

INCLUDE PERFETTO MODULE wattson.curves.device_gpu;

INCLUDE PERFETTO MODULE wattson.curves.device_l3;

INCLUDE PERFETTO MODULE wattson.device_infos;

-- 1D LUT
CREATE PERFETTO TABLE _filtered_curves_1d_raw AS
SELECT
  cp.policy,
  freq_khz,
  active,
  idle0,
  idle1,
  static
FROM _device_curves_1d AS dc
JOIN _wattson_device AS device
  ON dc.device = device.name
JOIN _dev_cpu_policy_map AS cp
  ON dc.policy = cp.policy;

-- Gets the active curve value dependency of the minimum frequency/voltage vote
CREATE PERFETTO TABLE _min_active_curve_value_for_dependency AS
SELECT
  active AS min_dependency
FROM _filtered_curves_1d_raw
ORDER BY
  freq_khz ASC
LIMIT 1;

CREATE PERFETTO TABLE _cpus_with_no_dependency AS
SELECT DISTINCT
  cpu
FROM _dev_cpu_policy_map
JOIN _filtered_curves_1d_raw
  USING (policy);

CREATE PERFETTO TABLE _cpus_with_dependency AS
WITH
  base AS (
    SELECT DISTINCT
      cpu
    FROM _dev_cpu_policy_map
    WHERE
      NOT cpu IN (
        SELECT
          cpu
        FROM _cpus_with_no_dependency
      )
  )
SELECT
  cpu
FROM base
UNION ALL
-- If no CPUs at all with 2D dependency, then the remaining CPUs (e.g. all CPUs
-- with no dependency) need to accounted for in static power calculations
SELECT
  cpu
FROM _cpus_with_no_dependency
WHERE
  NOT EXISTS(
    SELECT
      1
    FROM base
  );

-- Find the exemplar CPU for the policy with no freq/voltage dependency. The CPU
-- managing the policy is the first CPU that comes online for a given policy.
-- This is usually the min(CPU #) since Linux initializes CPUs in ascending
-- order (e.g. CPU4 for policy4).
CREATE PERFETTO TABLE _cpu_for_1d_static AS
SELECT
  min(cpu) AS cpu
FROM _cpus_with_no_dependency;

-- Find the exemplar CPU for the policy with a freq/voltage dependency. The CPU
-- managing the policy is the first CPU that comes online for a given policy.
-- This is usually the min(CPU #) since Linux initializes CPUs in ascending
-- order (e.g. CPU0 for policy0).
CREATE PERFETTO TABLE _cpu_for_2d_static AS
SELECT
  min(cpu) AS cpu
FROM _cpus_with_dependency;

CREATE PERFETTO TABLE _filtered_curves_1d AS
SELECT
  policy,
  freq_khz,
  -1 AS idle,
  active AS curve_value
FROM _filtered_curves_1d_raw
UNION
SELECT
  policy,
  freq_khz,
  0,
  idle0
FROM _filtered_curves_1d_raw
UNION
SELECT
  policy,
  freq_khz,
  1,
  idle1
FROM _filtered_curves_1d_raw
UNION
SELECT
  policy,
  freq_khz,
  255,
  static
FROM _filtered_curves_1d_raw AS c
JOIN _cpu_for_1d_static AS s
  ON s.cpu = c.policy;

CREATE PERFETTO INDEX freq_1d ON _filtered_curves_1d(policy, freq_khz, idle);

-- 2D LUT; with dependency on another CPU
CREATE PERFETTO TABLE _filtered_curves_2d_raw AS
SELECT
  dc.freq_khz,
  dc.dependency,
  dc.active,
  dc.idle0,
  dc.idle1,
  dc.static
FROM _device_curves_2d AS dc
JOIN _wattson_device AS device
  ON dc.device = device.name;

CREATE PERFETTO TABLE _filtered_curves_2d AS
SELECT
  freq_khz,
  dependency,
  -1 AS idle,
  active AS curve_value
FROM _filtered_curves_2d_raw
UNION
SELECT
  freq_khz,
  dependency,
  0,
  idle0
FROM _filtered_curves_2d_raw
UNION
SELECT
  freq_khz,
  dependency,
  1,
  idle1
FROM _filtered_curves_2d_raw
UNION
SELECT
  freq_khz,
  dependency,
  255,
  static
FROM _filtered_curves_2d_raw;

CREATE PERFETTO INDEX freq_2d ON _filtered_curves_2d(freq_khz, dependency, idle);

-- L3 cache LUT
CREATE PERFETTO TABLE _filtered_curves_l3_raw AS
SELECT
  dc.freq_khz,
  dc.dependency,
  dc.l3_hit,
  dc.l3_miss
FROM _device_curves_l3 AS dc
JOIN _wattson_device AS device
  ON dc.device = device.name;

CREATE PERFETTO TABLE _filtered_curves_l3 AS
SELECT
  freq_khz,
  dependency,
  'hit' AS action,
  l3_hit AS curve_value
FROM _filtered_curves_l3_raw
UNION
SELECT
  freq_khz,
  dependency,
  'miss' AS action,
  l3_miss
FROM _filtered_curves_l3_raw;

CREATE PERFETTO INDEX freq_l3 ON _filtered_curves_l3(freq_khz, dependency, action);

-- Device specific GPU curves
CREATE PERFETTO TABLE _gpu_filtered_curves_raw AS
SELECT
  freq_khz,
  active,
  idle1,
  idle2
FROM _gpu_device_curves AS dc
JOIN _wattson_device AS device
  ON dc.device = device.name;

CREATE PERFETTO TABLE _gpu_filtered_curves AS
SELECT
  freq_khz,
  2 AS idle,
  active AS curve_value
FROM _gpu_filtered_curves_raw
UNION ALL
SELECT
  freq_khz,
  1 AS idle,
  idle1 AS curve_value
FROM _gpu_filtered_curves_raw
UNION ALL
SELECT
  freq_khz,
  0 AS idle,
  idle2 AS curve_value
FROM _gpu_filtered_curves_raw;

CREATE PERFETTO INDEX gpu_freq ON _gpu_filtered_curves(freq_khz, idle);
