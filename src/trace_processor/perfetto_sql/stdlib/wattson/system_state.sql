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

INCLUDE PERFETTO MODULE wattson.cpu_split;

-- The final system state for the CPU subsystem, which has all the information
-- needed by Wattson to estimate energy for the CPU subsystem.
CREATE PERFETTO TABLE _wattson_system_states (
  -- Starting timestamp of the current counter where system state is constant.
  ts TIMESTAMP,
  -- Duration of the current counter where system state is constant.
  dur DURATION,
  -- Number of L3 hits the current system state.
  l3_hit_count LONG,
  -- Number of L3 misses in the current system state.
  l3_miss_count LONG,
  -- Frequency of CPU0.
  freq_0 LONG,
  -- Idle state of CPU0.
  idle_0 LONG,
  -- Frequency of CPU1.
  freq_1 LONG,
  -- Idle state of CPU1.
  idle_1 LONG,
  -- Frequency of CPU2.
  freq_2 LONG,
  -- Idle state of CPU2.
  idle_2 LONG,
  -- Frequency of CPU3.
  freq_3 LONG,
  -- Idle state of CPU3.
  idle_3 LONG,
  -- Frequency of CPU4.
  freq_4 LONG,
  -- Idle state of CPU4.
  idle_4 LONG,
  -- Frequency of CPU5.
  freq_5 LONG,
  -- Idle state of CPU5.
  idle_5 LONG,
  -- Frequency of CPU6.
  freq_6 LONG,
  -- Idle state of CPU6.
  idle_6 LONG,
  -- Frequency of CPU7.
  freq_7 LONG,
  -- Idle state of CPU7.
  idle_7 LONG,
  -- Flag indicating if current system state is suspended.
  suspended BOOL
) AS
SELECT
  s.ts,
  s.dur,
  cast_int!(round(l3_hit_rate * s.dur, 0)) AS l3_hit_count,
  cast_int!(round(l3_miss_rate * s.dur, 0)) AS l3_miss_count,
  freq_0,
  idle_0,
  freq_1,
  idle_1,
  freq_2,
  idle_2,
  freq_3,
  idle_3,
  freq_4,
  idle_4,
  freq_5,
  idle_5,
  freq_6,
  idle_6,
  freq_7,
  idle_7,
  coalesce(suspended, FALSE) AS suspended
FROM _idle_freq_l3_hit_l3_miss_slice AS s
JOIN _stats_cpu0
  ON _stats_cpu0._auto_id = s.cpu0_id
JOIN _stats_cpu1
  ON _stats_cpu1._auto_id = s.cpu1_id
JOIN _stats_cpu2
  ON _stats_cpu2._auto_id = s.cpu2_id
JOIN _stats_cpu3
  ON _stats_cpu3._auto_id = s.cpu3_id
LEFT JOIN _stats_cpu4
  ON _stats_cpu4._auto_id = s.cpu4_id
LEFT JOIN _stats_cpu5
  ON _stats_cpu5._auto_id = s.cpu5_id
LEFT JOIN _stats_cpu6
  ON _stats_cpu6._auto_id = s.cpu6_id
LEFT JOIN _stats_cpu7
  ON _stats_cpu7._auto_id = s.cpu7_id
-- Needs to be at least 1us to reduce inconsequential rows.
WHERE
  s.dur > time_from_us(1);
