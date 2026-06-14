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

INCLUDE PERFETTO MODULE android.device;

INCLUDE PERFETTO MODULE wattson.utils;

-- Device specific info for deep idle time offsets
CREATE PERFETTO PIPELINE _device_cpu_deep_idle_offsets MATERIALIZED AS
FROM (
  VALUES
    ("Tensor", 0, 0), ("Tensor", 1, 0), ("Tensor", 2, 0), ("Tensor", 3, 0),
    ("Tensor", 4, 0), ("Tensor", 5, 0), ("Tensor", 6, 200000), ("Tensor", 7, 200000),
    ("monaco", 0, 450000), ("monaco", 1, 450000), ("monaco", 2, 450000), ("monaco", 3, 450000),
    ("Tensor G4", 0, 0), ("Tensor G4", 1, 0), ("Tensor G4", 2, 0), ("Tensor G4", 3, 0),
    ("Tensor G4", 4, 110000), ("Tensor G4", 5, 110000), ("Tensor G4", 6, 110000),
    ("Tensor G4", 7, 400000), ("Tensor G5", 0, 0), ("Tensor G5", 1, 0), ("Tensor G5", 2, 0),
    ("Tensor G5", 3, 0), ("Tensor G5", 4, 0), ("Tensor G5", 5, 0), ("Tensor G5", 6, 0),
    ("Tensor G5", 7, 0), ("neo", 0, 100000), ("neo", 1, 100000), ("neo", 2, 100000),
    ("neo", 3, 100000), ("SXR2230P", 0, 0), ("SXR2230P", 1, 0), ("SXR2230P", 2, 0),
    ("SXR2230P", 3, 0), ("SXR2230P", 4, 0), ("SXR2230P", 5, 0), ("MT6858", 0, 0),
    ("MT6858", 1, 0), ("MT6858", 2, 0), ("MT6858", 3, 0), ("MT6858", 4, 0),
    ("MT6858", 5, 0), ("MT6858", 6, 0), ("MT6858", 7, 0), ("MT6897", 0, 0),
    ("MT6897", 1, 0), ("MT6897", 2, 0), ("MT6897", 3, 0), ("MT6897", 4, 0),
    ("MT6897", 5, 0), ("MT6897", 6, 0), ("MT6897", 7, 0), ("SM8750", 0, 0),
    ("SM8750", 1, 0), ("SM8750", 2, 0), ("SM8750", 3, 0), ("SM8750", 4, 0),
    ("SM8750", 5, 0), ("SM8750", 6, 0), ("SM8750", 7, 0)
) AS _values(device, cpu, offset_ns);

CREATE PERFETTO PIPELINE _wattson_device_map MATERIALIZED AS
FROM (
  VALUES
    ("oriole", "Tensor"),
    ("raven", "Tensor"),
    ("bluejay", "Tensor"),
    ("eos", "monaco"),
    ("aurora", "monaco")
) AS _values(device, wattson_device);

CREATE PERFETTO PIPELINE _wattson_device MATERIALIZED AS
SUBPIPELINE soc_model AS (
  FROM (
    SELECT
      coalesce(
        -- Get guest model from metadata, which takes precedence if set
        (
          SELECT str_value
          FROM metadata
          WHERE name = 'android_guest_soc_model'
          LIMIT 1
        ),
        -- Get model from metadata
        (
          SELECT str_value
          FROM metadata
          WHERE name = 'android_soc_model'
          LIMIT 1
        ),
        -- Get device name from metadata and map it to model
        (
          SELECT wattson_device
          FROM _wattson_device_map AS map
          JOIN android_device_name AS ad
            ON ad.name = map.device
        )
      ) AS name
  )
)
-- Once model is obtained, check to see if the model is supported by Wattson
-- via checking if model is within a key-value pair mapping
FROM soc_model
|> JOIN _device_cpu_deep_idle_offsets AS map ON map.device = soc_model.name
|> SELECT DISTINCT soc_model.name AS name;

-- Device specific mapping from CPU to policy
CREATE PERFETTO PIPELINE _cpu_to_policy_map MATERIALIZED AS
FROM (
  VALUES
    ("monaco", 0, 0), ("monaco", 1, 0), ("monaco", 2, 0), ("monaco", 3, 0),
    ("Tensor", 0, 0), ("Tensor", 1, 0), ("Tensor", 2, 0), ("Tensor", 3, 0),
    ("Tensor", 4, 4), ("Tensor", 5, 4), ("Tensor", 6, 6), ("Tensor", 7, 6),
    ("Tensor G4", 0, 0), ("Tensor G4", 1, 0), ("Tensor G4", 2, 0), ("Tensor G4", 3, 0),
    ("Tensor G4", 4, 4), ("Tensor G4", 5, 4), ("Tensor G4", 6, 4), ("Tensor G4", 7, 7),
    ("Tensor G5", 0, 0), ("Tensor G5", 1, 0), ("Tensor G5", 2, 2), ("Tensor G5", 3, 2),
    ("Tensor G5", 4, 2), ("Tensor G5", 5, 5), ("Tensor G5", 6, 5), ("Tensor G5", 7, 7),
    ("neo", 0, 0), ("neo", 1, 0), ("neo", 2, 0), ("neo", 3, 0), ("SXR2230P", 0, 0),
    ("SXR2230P", 1, 0), ("SXR2230P", 2, 2), ("SXR2230P", 3, 2), ("SXR2230P", 4, 2),
    ("SXR2230P", 5, 2), ("MT6858", 0, 0), ("MT6858", 1, 0), ("MT6858", 2, 0),
    ("MT6858", 3, 0), ("MT6858", 4, 4), ("MT6858", 5, 4), ("MT6858", 6, 4),
    ("MT6858", 7, 4), ("MT6897", 0, 0), ("MT6897", 1, 0), ("MT6897", 2, 0),
    ("MT6897", 3, 0), ("MT6897", 4, 4), ("MT6897", 5, 4), ("MT6897", 6, 4),
    ("MT6897", 7, 7), ("SM8750", 0, 0), ("SM8750", 1, 0), ("SM8750", 2, 0),
    ("SM8750", 3, 0), ("SM8750", 4, 0), ("SM8750", 5, 0), ("SM8750", 6, 6),
    ("SM8750", 7, 6)
) AS _values(device, cpu, policy);

-- Prefilter table based on device
CREATE PERFETTO PIPELINE _dev_cpu_policy_map MATERIALIZED AS
FROM _cpu_to_policy_map AS cp_map
|> JOIN _wattson_device AS device ON cp_map.device = device.name
|> SELECT cpu, policy
|> ORDER BY cpu;

-- Identifies unique policies on this device
CREATE PERFETTO PIPELINE _device_policies MATERIALIZED AS
FROM _dev_cpu_policy_map
|> SELECT DISTINCT policy;

-- Defines bitmasks for each CPU where bits are set for all cores sharing the same
-- cpufreq policy. This is used to determine if a cluster is active (any core on)
-- to correctly attribute shared static power.
CREATE PERFETTO PIPELINE _policy_masks MATERIALIZED AS
FROM _dev_cpu_policy_map
|> AGGREGATE
     max(iif(cpu = 0, policy, -1)) AS p0,
     max(iif(cpu = 1, policy, -1)) AS p1,
     max(iif(cpu = 2, policy, -1)) AS p2,
     max(iif(cpu = 3, policy, -1)) AS p3,
     max(iif(cpu = 4, policy, -1)) AS p4,
     max(iif(cpu = 5, policy, -1)) AS p5,
     max(iif(cpu = 6, policy, -1)) AS p6,
     max(iif(cpu = 7, policy, -1)) AS p7
|> SELECT
     _policy_mask!(p0, p0, p1, p2, p3, p4, p5, p6, p7) AS m0,
     _policy_mask!(p1, p0, p1, p2, p3, p4, p5, p6, p7) AS m1,
     _policy_mask!(p2, p0, p1, p2, p3, p4, p5, p6, p7) AS m2,
     _policy_mask!(p3, p0, p1, p2, p3, p4, p5, p6, p7) AS m3,
     _policy_mask!(p4, p0, p1, p2, p3, p4, p5, p6, p7) AS m4,
     _policy_mask!(p5, p0, p1, p2, p3, p4, p5, p6, p7) AS m5,
     _policy_mask!(p6, p0, p1, p2, p3, p4, p5, p6, p7) AS m6,
     _policy_mask!(p7, p0, p1, p2, p3, p4, p5, p6, p7) AS m7;

-- Devices that require using devfreq
CREATE PERFETTO PIPELINE _use_devfreq MATERIALIZED AS
FROM (VALUES ("Tensor G4"), ("Tensor G5")) AS _values(device);

-- Creates non-empty table if device needs devfreq
CREATE PERFETTO PIPELINE _use_devfreq_for_calc MATERIALIZED AS
FROM _use_devfreq AS d
|> JOIN _wattson_device AS device ON d.device = device.name
|> SELECT TRUE AS devfreq_necessary;

-- Creates empty table if device needs devfreq; inverse of _use_devfreq_for_calc
CREATE PERFETTO PIPELINE _skip_devfreq_for_calc MATERIALIZED AS
FROM (SELECT FALSE AS devfreq_necessary)
|> WHERE NOT EXISTS (SELECT * FROM _use_devfreq_for_calc);

-- Devices that require idle state mapping
CREATE PERFETTO PIPELINE _idle_state_map MATERIALIZED AS
FROM (
  VALUES
    ("MT6858", 4294967295, -1), ("MT6858", 0, 0), ("MT6858", 1, 1),
    ("MT6858", 2, 1), ("MT6858", 3, 1), ("MT6858", 4, 1), ("MT6858", 5, 1),
    ("MT6897", 4294967295, -1), ("MT6897", 0, 0), ("MT6897", 1, 1), ("MT6897", 2, 1),
    ("MT6897", 3, 1), ("MT6897", 4, 1), ("MT6897", 5, 1), ("MT6897", 6, 1),
    ("MT6897", 7, 1), ("MT6897", 8, 1), ("neo", 4294967295, -1), ("neo", 0, 0),
    ("neo", 1, 1), ("neo", 2, 1), ("SXR2230P", 4294967295, -1), ("SXR2230P", 0, 0),
    ("SXR2230P", 1, 1), ("SXR2230P", 2, 1)
) AS _values(device, nominal_idle, override_idle);

-- idle_mapping override filtered for device
CREATE PERFETTO PIPELINE _idle_state_map_override MATERIALIZED AS
FROM _idle_state_map AS idle_map
|> JOIN _wattson_device AS device ON idle_map.device = device.name
|> SELECT nominal_idle, override_idle;

-- Get the device specific deepest idle state if defined, otherwise use 1 as the
-- deepest idle state
CREATE PERFETTO PIPELINE _deepest_idle MATERIALIZED AS
FROM (
  SELECT
    coalesce((SELECT max(override_idle) FROM _idle_state_map_override), 1) AS idle
);

-- Specify which device-cpu combination has 2D dependency that votes by
-- frequency (as opposed to the default, vote by power)
CREATE PERFETTO PIPELINE _vote_by_freq MATERIALIZED AS
FROM (VALUES ("Tensor G5", 5), ("Tensor G5", 6), ("Tensor G5", 7)) AS _values(device, cpu);

-- Gets all CPUs on device and whether the CPU vote is be freq or power
CREATE PERFETTO PIPELINE _dev_vote_by_freq MATERIALIZED AS
FROM _dev_cpu_policy_map AS m
|> LEFT JOIN _vote_by_freq AS v USING (cpu)
|> WHERE v.cpu IS NULL
|> SELECT m.cpu AS cpu, 0 AS vote_by_freq
|> UNION ALL (
     FROM _vote_by_freq AS v
     |> JOIN _wattson_device AS device ON v.device = device.name
     |> SELECT v.cpu AS cpu, 1 AS vote_by_freq
   )
|> SELECT cpu, vote_by_freq
|> ORDER BY cpu;

-- Device specific mapping to GPU ID
CREATE PERFETTO PIPELINE _gpuid_map MATERIALIZED AS
FROM (VALUES ("Tensor G5", 0), ("Tensor", 1)) AS _values(device, gpu_id);
