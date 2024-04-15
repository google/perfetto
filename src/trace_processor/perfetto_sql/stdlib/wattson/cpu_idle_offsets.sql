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

-- Device specific info for deep idle time offsets
CREATE PERFETTO TABLE _device_cpu_deep_idle_offsets
AS
WITH data(device, cpu, offset_ns) AS (
  VALUES
  ("oriole", 6, 200000),
  ("oriole", 7, 200000),
  ("raven", 6, 200000),
  ("raven", 7, 200000),
  ("eos", 0, 450000),
  ("eos", 1, 450000),
  ("eos", 2, 450000),
  ("eos", 3, 450000)
)
select * from data;

