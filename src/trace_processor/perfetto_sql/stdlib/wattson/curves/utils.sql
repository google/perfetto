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

INCLUDE PERFETTO MODULE wattson.curves.cpu_1d;

INCLUDE PERFETTO MODULE wattson.curves.cpu_2d;

INCLUDE PERFETTO MODULE wattson.curves.gpu;

INCLUDE PERFETTO MODULE wattson.curves.l3;

INCLUDE PERFETTO MODULE wattson.curves.tpu;

INCLUDE PERFETTO MODULE wattson.device_infos;

INCLUDE PERFETTO MODULE wattson.utils;

-- 1D LUT
CREATE PERFETTO PIPELINE _filtered_curves_1d_raw MATERIALIZED AS
FROM _device_curves_1d AS dc
|> JOIN _wattson_device AS device
   ON dc.device = device.name
|> JOIN _dev_cpu_policy_map AS cp
   ON dc.policy = cp.policy
|> SELECT cp.policy, freq_khz, active, idle0, idle1, static;

CREATE PERFETTO PIPELINE _filtered_curves_1d MATERIALIZED AS
SUBPIPELINE i0 AS (
  FROM _filtered_curves_1d_raw
  |> SELECT policy, freq_khz, 0 AS idle, idle0 AS curve_value, static
)
SUBPIPELINE i1 AS (
  FROM _filtered_curves_1d_raw
  |> SELECT policy, freq_khz, 1 AS idle, idle1 AS curve_value, static
)
FROM _filtered_curves_1d_raw
|> SELECT policy, freq_khz, -1 AS idle, active AS curve_value, static
|> UNION (FROM i0)
|> UNION (FROM i1);

CREATE PERFETTO INDEX freq_1d ON _filtered_curves_1d(policy, freq_khz, idle);

-- 2D LUT; with dependency on another CPU
CREATE PERFETTO PIPELINE _filtered_curves_2d_raw MATERIALIZED AS
FROM _device_curves_2d AS dc
|> JOIN _wattson_device AS device
   ON dc.device = device.name
|> SELECT
  dc.policy,
  dc.freq_khz,
  dc.dep_policy,
  dc.dep_freq,
  dc.active,
  dc.idle0,
  dc.idle1,
  dc.static,
  dc.interconnect;

CREATE PERFETTO PIPELINE _filtered_curves_2d MATERIALIZED AS
SUBPIPELINE i0 AS (
  FROM _filtered_curves_2d_raw
  |> SELECT freq_khz, dep_policy, dep_freq, 0 AS idle, static, idle0 AS curve_value
)
SUBPIPELINE i1 AS (
  FROM _filtered_curves_2d_raw
  |> SELECT freq_khz, dep_policy, dep_freq, 1 AS idle, static, idle1 AS curve_value
)
FROM _filtered_curves_2d_raw
|> SELECT freq_khz, dep_policy, dep_freq, -1 AS idle, static, active AS curve_value
|> UNION (FROM i0)
|> UNION (FROM i1);

CREATE PERFETTO INDEX freq_2d ON _filtered_curves_2d(
  freq_khz,
  dep_policy,
  dep_freq,
  idle
);

-- L3 cache LUT
CREATE PERFETTO PIPELINE _filtered_curves_l3 MATERIALIZED AS
FROM _device_curves_l3 AS dc
|> JOIN _wattson_device AS device
   ON dc.device = device.name
|> SELECT dc.freq_khz, dc.dep_policy, dc.dep_freq, dc.l3_hit, dc.l3_miss;

CREATE PERFETTO INDEX freq_l3 ON _filtered_curves_l3(
  freq_khz,
  dep_policy,
  dep_freq
);

-- Device specific GPU curves
CREATE PERFETTO PIPELINE _gpu_filtered_curves_raw MATERIALIZED AS
FROM _gpu_device_curves AS dc
|> JOIN _wattson_device AS device
   ON dc.device = device.name
|> SELECT freq_khz, active, idle1, idle2;

CREATE PERFETTO PIPELINE _gpu_filtered_curves MATERIALIZED AS
SUBPIPELINE i1 AS (
  FROM _gpu_filtered_curves_raw
  |> SELECT freq_khz, 1 AS idle, idle1 AS curve_value
)
SUBPIPELINE i0 AS (
  FROM _gpu_filtered_curves_raw
  |> SELECT freq_khz, 0 AS idle, idle2 AS curve_value
)
FROM _gpu_filtered_curves_raw
|> SELECT freq_khz, 2 AS idle, active AS curve_value
|> UNION ALL (FROM i1)
|> UNION ALL (FROM i0);

CREATE PERFETTO INDEX gpu_freq ON _gpu_filtered_curves(freq_khz, idle);

-- Device specific TPU curves
CREATE PERFETTO PIPELINE _tpu_filtered_curves MATERIALIZED AS
FROM _tpu_device_curves AS dc
|> JOIN _wattson_device AS device
   ON dc.device = device.name
|> SELECT cluster, freq, requests, active;

CREATE PERFETTO INDEX tpu_curves ON _tpu_filtered_curves(
  cluster,
  freq,
  requests
);

-- Device specific interconnect curves
CREATE PERFETTO PIPELINE _filtered_curves_interconnect MATERIALIZED AS
FROM _filtered_curves_2d_raw AS dc
|> WHERE dc.interconnect > 0
|> SELECT
  dc.policy,
  dc.freq_khz,
  dc.dep_policy,
  dc.dep_freq,
  dc.interconnect AS curve_value;

CREATE PERFETTO INDEX freq_interconnect ON _filtered_curves_interconnect(
  policy,
  freq_khz,
  dep_policy,
  dep_freq
);

-- Constructs table specifying CPUs that are DSU dependent
CREATE PERFETTO PIPELINE _cpu_w_dsu_dependency MATERIALIZED AS
FROM _filtered_curves_2d_raw
|> JOIN _dev_cpu_policy_map USING (policy)
|> WHERE dep_policy = _dsu_dep!()
|> SELECT DISTINCT cpu;

-- Chooses the minimum vote for CPUs with dependencies
CREATE PERFETTO PIPELINE _cpu_w_dependency_default_vote MATERIALIZED AS
FROM _filtered_curves_2d_raw
|> AGGREGATE ANY_VALUE(dep_policy) AS dep_policy, min(dep_freq) AS dep_freq GROUP BY policy
|> JOIN _dev_cpu_policy_map USING (policy)
|> SELECT cpu, dep_policy, dep_freq;

-- CPUs that need to be checked for static calculation
CREATE PERFETTO PIPELINE _cpus_for_static MATERIALIZED AS
SUBPIPELINE from_1d AS (
  FROM _filtered_curves_1d AS c
  |> JOIN _dev_cpu_policy_map AS m USING (policy)
  |> WHERE static > 0
  |> SELECT DISTINCT m.cpu
)
FROM _filtered_curves_2d_raw AS c
|> JOIN _dev_cpu_policy_map AS m USING (policy)
|> WHERE static > 0
|> SELECT DISTINCT m.cpu
|> UNION (FROM from_1d);

-- Contructs table specifying CPU dependency of each CPU (if applicable)
CREATE PERFETTO PIPELINE _cpu_lut_dependencies MATERIALIZED AS
SUBPIPELINE base_cpus AS (
  FROM _filtered_curves_2d_raw AS c
  |> JOIN _dev_cpu_policy_map AS m USING (policy)
  |> WHERE dep_policy != _dsu_dep!()
  |> SELECT DISTINCT m.cpu, m.policy
)
SUBPIPELINE dep_cpus AS (
  FROM _filtered_curves_2d_raw AS c
  |> JOIN _dev_cpu_policy_map AS m
     ON c.dep_policy = m.policy
  |> SELECT DISTINCT m.cpu AS dep_cpu, m.policy AS dep_policy
)
FROM base_cpus AS b
|> JOIN dep_cpus AS d ON b.policy != d.dep_policy
|> SELECT b.cpu, d.dep_cpu;
