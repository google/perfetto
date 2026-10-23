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

INCLUDE PERFETTO MODULE linux.cpu.frequency;

INCLUDE PERFETTO MODULE wattson.device_infos;

-- Every physically present CPU's frequency from trace start to trace end, NULL
-- where no freq event defines it, so that interval_intersect doesn't remove the
-- undefined time region.
CREATE PERFETTO TABLE _adjusted_cpu_freq AS
FROM (
  SELECT ts, dur, freq, cf.ucpu AS cpu, d_map.policy
  FROM cpu_frequency_counters AS cf
  JOIN _dev_cpu_policy_map AS d_map
    ON cf.ucpu = d_map.cpu
)
|> INTERVAL FILL GAPS WITH (
  SELECT trace_start() AS ts, trace_dur() AS dur, cpu, policy
  FROM _dev_cpu_policy_map
) PER cpu;
