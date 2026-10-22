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

INCLUDE PERFETTO MODULE android.suspend;

INCLUDE PERFETTO MODULE time.conversion;

INCLUDE PERFETTO MODULE wattson.device_infos;

-- Creates the hotplug slice(s) for each CPU defined to be the region when CPU
-- is off
CREATE PERFETTO TABLE _cpu_hotplug_offline AS
WITH
  is_different_cpu AS (
    -- Set flag for when hotplug CPU processing is being done on separate CPU
    SELECT
      s.ts,
      s.dur,
      extract_arg(t.dimension_arg_set_id, 'cpu') AS hp_cpu,
      extract_arg(t.dimension_arg_set_id, 'cpu')
      != extract_arg(s.arg_set_id, 'action_cpu') AS is_different_cpu
    FROM slice AS s
    JOIN track AS t
      ON t.id = s.track_id
    WHERE
      t.type = 'cpu_hotplug'
  ),
  cpu_transitions AS (
    -- AP CPU is CPU being hotplugged out, and BP CPU is CPU that assists the AP
    -- CPU in hotplugging. The BP CPU could be the AP CPU itself or a different
    -- CPU. Find the transition points where the BP CPU changes between AP and BP.
    SELECT
      ts,
      hp_cpu,
      is_different_cpu,
      lag(is_different_cpu) OVER (PARTITION BY hp_cpu) != is_different_cpu AS is_cpu_transitions
    FROM is_different_cpu
  ),
  transitions_dur AS (
    -- Calculates duration between transitions from AP -> BP and BP -> AP
    SELECT
      ts,
      lead(ts, 1, trace_end()) OVER (PARTITION BY hp_cpu) - ts AS dur,
      hp_cpu AS cpu,
      is_different_cpu
    FROM cpu_transitions
    WHERE
      is_cpu_transitions
  )
SELECT
  ts,
  dur,
  cpu,
  -- Sometimes the assignment of AP CPU during hotplugging creates short,
  -- spurious "pockets" of hotplug events, so assign these slices that are
  -- shorter than 100us as if they were on the same CPU.
  iif(is_different_cpu AND dur < time_from_us(100), FALSE, is_different_cpu) AS is_different_cpu
FROM transitions_dur;

-- Every CPU's hotplug state from trace start to trace end, online wherever
-- cpuhp says nothing, so that this table can be used by interval_intersect():
-- a CPU which is never offline is online throughout.
CREATE PERFETTO TABLE _gapless_hotplug_slices AS
FROM (
  SELECT ts, dur, cpu, is_different_cpu AS offline FROM _cpu_hotplug_offline
)
|> INTERVAL FILL GAPS WITH (
  SELECT trace_start() AS ts, trace_dur() AS dur, cpu, FALSE AS offline
  FROM _dev_cpu_policy_map
) PER cpu
|> ORDER BY cpu, ts;

-- Nominal suspend table with gapless suspended state.
CREATE PERFETTO TABLE _gapless_suspend_slices AS
SELECT ts, dur, iif(power_state = 'suspended', TRUE, FALSE) AS suspended
FROM android_suspend_state;
