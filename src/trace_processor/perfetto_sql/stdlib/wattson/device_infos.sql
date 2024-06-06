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

-- Device specific info for deep idle time offsets
CREATE PERFETTO TABLE _device_cpu_deep_idle_offsets
AS
WITH data(device, cpu, offset_ns) AS (
  VALUES
  ("Tensor", 0, 0),
  ("Tensor", 1, 0),
  ("Tensor", 2, 0),
  ("Tensor", 3, 0),
  ("Tensor", 4, 0),
  ("Tensor", 5, 0),
  ("Tensor", 6, 200000),
  ("Tensor", 7, 200000),
  ("monaco", 0, 450000),
  ("monaco", 1, 450000),
  ("monaco", 2, 450000),
  ("monaco", 3, 450000)
)
select * from data;

CREATE PERFETTO TABLE _wattson_device_map
AS
WITH data(device, wattson_device) AS (
  VALUES
  ("oriole", "Tensor"),
  ("raven", "Tensor"),
  ("bluejay", "Tensor"),
  ("eos", "monaco")
)
select * from data;

CREATE PERFETTO TABLE _wattson_device
AS
WITH soc AS (
  SELECT str_value as model
  FROM metadata
  WHERE name = 'android_soc_model'
)
SELECT
  COALESCE(soc.model, map.wattson_device) as name
FROM _wattson_device_map as map
CROSS JOIN android_device_name as ad
LEFT JOIN soc ON TRUE
WHERE ad.name = map.device;

