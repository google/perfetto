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

CREATE PERFETTO PIPELINE _adjusted_cpu_freq MATERIALIZED AS
-- The real per-CPU freq intervals, keyed by cpu.
FROM cpu_frequency_counters AS cf
|> JOIN _dev_cpu_policy_map AS d_map ON cf.ucpu = d_map.cpu
|> SELECT ts, dur, freq, cf.ucpu AS cpu, d_map.policy
|> FORK AS cpu_freq
-- Pass the real intervals through and fill the leading gap (trace start up to
-- the first freq event) per CPU with a null-freq filler. The filler's policy is
-- re-attached from the policy map below.
FROM cpu_freq
|> INTERVAL FILL WITHIN trace_bounds PER cpu
|> LEFT JOIN _dev_cpu_policy_map AS d_map USING (cpu)
|> SELECT ts, dur, freq, cpu, d_map.policy AS policy
|> UNION ALL (
     -- Add empty cpu freq counters for CPUs that are physically present, but
     -- did not have a single freq event register. The time region needs to be
     -- defined so that interval_intersect doesn't remove the undefined region.
     FROM _dev_cpu_policy_map
     |> WHERE NOT (cpu IN (SELECT cpu FROM cpu_freq))
     |> SELECT
          trace_start() AS ts,
          trace_dur() AS dur,
          NULL AS freq,
          cpu,
          NULL AS policy
   );
