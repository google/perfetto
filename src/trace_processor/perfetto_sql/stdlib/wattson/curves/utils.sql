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

INCLUDE PERFETTO MODULE wattson.curves.device;
INCLUDE PERFETTO MODULE wattson.device_infos;

-- 1D LUT
CREATE PERFETTO TABLE _filtered_curves_1d_raw AS
SELECT cp.policy, freq_khz, active, idle0, idle1, static
FROM _device_curves_1d as dc
JOIN _wattson_device as device ON dc.device = device.name
JOIN _dev_cpu_policy_map as cp ON dc.policy = cp.policy;

CREATE PERFETTO TABLE _filtered_curves_1d AS
SELECT policy, freq_khz, -1 as idle, active as curve_value
FROM _filtered_curves_1d_raw
UNION
SELECT policy, freq_khz, 0, idle0
FROM _filtered_curves_1d_raw
UNION
SELECT policy, freq_khz, 1, idle1
FROM _filtered_curves_1d_raw
UNION
SELECT policy, freq_khz, 255, static
FROM _filtered_curves_1d_raw;

CREATE PERFETTO INDEX freq_1d ON _filtered_curves_1d(policy, freq_khz, idle);

-- 2D LUT; with dependency on another CPU
CREATE PERFETTO TABLE _filtered_curves_2d_raw AS
SELECT
  cp.policy as other_policy,
  dc.freq_khz,
  dc.other_freq_khz,
  dc.active,
  dc.idle0,
  dc.idle1,
  dc.static
FROM _device_curves_2d as dc
JOIN _wattson_device as device ON dc.device = device.name
JOIN _dev_cpu_policy_map as cp ON dc.other_policy = cp.policy;

CREATE PERFETTO TABLE _filtered_curves_2d AS
SELECT freq_khz, other_policy, other_freq_khz, -1 as idle, active as curve_value
FROM _filtered_curves_2d_raw
UNION
SELECT freq_khz, other_policy, other_freq_khz, 0, idle0
FROM _filtered_curves_2d_raw
UNION
SELECT freq_khz, other_policy, other_freq_khz, 1, idle1
FROM _filtered_curves_2d_raw
UNION
SELECT freq_khz, other_policy, other_freq_khz, 255, static
FROM _filtered_curves_2d_raw;

CREATE PERFETTO INDEX freq_2d
ON _filtered_curves_2d(freq_khz, other_policy, other_freq_khz, idle);

-- L3 cache LUT
CREATE PERFETTO TABLE _filtered_curves_l3_raw AS
SELECT
  cp.policy as other_policy,
  dc.freq_khz,
  dc.other_freq_khz,
  dc.l3_hit,
  dc.l3_miss
FROM _device_curves_l3 as dc
JOIN _wattson_device as device ON dc.device = device.name
JOIN _dev_cpu_policy_map as cp ON dc.other_policy = cp.policy;

CREATE PERFETTO TABLE _filtered_curves_l3 AS
SELECT
  freq_khz, other_policy, other_freq_khz, 'hit' AS action, l3_hit as curve_value
FROM _filtered_curves_l3_raw
UNION
SELECT
  freq_khz, other_policy, other_freq_khz, 'miss' AS action, l3_miss
FROM _filtered_curves_l3_raw;

CREATE PERFETTO INDEX freq_l3
ON _filtered_curves_l3(freq_khz, other_policy, other_freq_khz, action);
