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

INCLUDE PERFETTO MODULE time.conversion;
INCLUDE PERFETTO MODULE wattson.arm_dsu;
INCLUDE PERFETTO MODULE wattson.cpu_freq;
INCLUDE PERFETTO MODULE wattson.cpu_idle;

-- Combines idle and freq tables of all CPUs to create system state.
CREATE VIRTUAL TABLE _idle_freq_slice
USING
  SPAN_OUTER_JOIN(_cpu_freq_all, _cpu_idle_all);

-- get suspend resume state as logged by ftrace.
CREATE PERFETTO TABLE _suspend_slice
AS
SELECT
  ts, dur, TRUE AS suspended
FROM slice
WHERE name GLOB "timekeeping_freeze(0)";

-- Combine suspend information with CPU idle and frequency system states.
CREATE VIRTUAL TABLE _idle_freq_suspend_slice
USING
  SPAN_OUTER_JOIN(_idle_freq_slice, _suspend_slice);

-- Add extra column indicating that idle and frequency info are present before
-- SPAN_OUTER_JOIN with the DSU PMU counters.
CREATE PERFETTO TABLE _idle_freq_filtered
AS
SELECT *, TRUE AS has_idle_freq
FROM _idle_freq_suspend_slice
WHERE freq_0 GLOB '*[0-9]*';

-- Combine system state so that it has idle, freq, and L3 hit info.
CREATE VIRTUAL TABLE _idle_freq_l3_hit_slice
USING
  SPAN_OUTER_JOIN(_idle_freq_filtered, _arm_l3_hit_rate);

-- Combine system state so that it has idle, freq, L3 hit, and L3 miss info.
CREATE VIRTUAL TABLE _idle_freq_l3_hit_l3_miss_slice
USING
  SPAN_OUTER_JOIN(_idle_freq_l3_hit_slice, _arm_l3_miss_rate);

-- The final system state for the CPU subsystem, which has all the information
-- needed by Wattson to estimate energy for the CPU subsystem.
CREATE PERFETTO TABLE wattson_system_states(
  -- Starting timestamp of the current counter where system state is constant.
  ts LONG,
  -- Duration of the current counter where system state is constant.
  dur INT,
  -- Number of L3 hits the current system state.
  l3_hit_count INT,
  -- Number of L3 misses in the current system state.
  l3_miss_count INT,
  -- Frequency of CPU0.
  freq_0 INT,
  -- Idle state of CPU0.
  idle_0 INT,
  -- Frequency of CPU1.
  freq_1 INT,
  -- Idle state of CPU1.
  idle_1 INT,
  -- Frequency of CPU2.
  freq_2 INT,
  -- Idle state of CPU2.
  idle_2 INT,
  -- Frequency of CPU3.
  freq_3 INT,
  -- Idle state of CPU3.
  idle_3 INT,
  -- Frequency of CPU4.
  freq_4 INT,
  -- Idle state of CPU4.
  idle_4 INT,
  -- Frequency of CPU5.
  freq_5 INT,
  -- Idle state of CPU5.
  idle_5 INT,
  -- Frequency of CPU6.
  freq_6 INT,
  -- Idle state of CPU6.
  idle_6 INT,
  -- Frequency of CPU7.
  freq_7 INT,
  -- Idle state of CPU7.
  idle_7 INT,
  -- Flag indicating if current system state is suspended.
  suspended BOOL
)
AS
SELECT
  ts,
  dur,
  cast_int!(round(l3_hit_rate * dur, 0)) as l3_hit_count,
  cast_int!(round(l3_miss_rate * dur, 0)) as l3_miss_count,
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
  IFNULL(suspended, FALSE) as suspended
FROM _idle_freq_l3_hit_l3_miss_slice
-- Needs to be at least 1us to reduce inconsequential rows.
WHERE dur > time_from_us(1) and has_idle_freq IS NOT NULL;

