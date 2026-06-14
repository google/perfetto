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
CREATE PERFETTO PIPELINE _cpu_hotplug_offline MATERIALIZED AS
-- Set flag for when hotplug CPU processing is being done on separate CPU
FROM slice AS s
|> JOIN track AS t ON t.id = s.track_id
|> WHERE t.type = 'cpu_hotplug'
|> SELECT
     s.ts,
     s.dur,
     extract_arg(t.dimension_arg_set_id, 'cpu') AS hp_cpu,
     extract_arg(t.dimension_arg_set_id, 'cpu')
     != extract_arg(s.arg_set_id, 'action_cpu') AS is_different_cpu
-- AP CPU is CPU being hotplugged out, and BP CPU is CPU that assists the AP
-- CPU in hotplugging. The BP CPU could be the AP CPU itself or a different
-- CPU. Find the transition points where the BP CPU changes between AP and BP.
|> SELECT
     ts,
     hp_cpu,
     is_different_cpu,
     lag(is_different_cpu) OVER (PARTITION BY hp_cpu) != is_different_cpu AS is_cpu_transitions
-- Calculates duration between transitions from AP -> BP and BP -> AP
|> WHERE is_cpu_transitions
|> SELECT
     ts,
     lead(ts, 1, trace_end()) OVER (PARTITION BY hp_cpu) - ts AS dur,
     hp_cpu AS cpu,
     is_different_cpu
-- Sometimes the assignment of AP CPU during hotplugging creates short,
-- spurious "pockets" of hotplug events, so assign these slices that are
-- shorter than 100us as if they were on the same CPU.
|> SET is_different_cpu = iif(is_different_cpu AND dur < time_from_us(100), FALSE, is_different_cpu);

-- Fill gaps from beginning of trace to end of trace so that this table can be
-- used by interval_intersect().
--
-- INTERVAL FILL covers, per CPU, the spans the cpuhp slices do not (the head
-- before the first offline slice and any tail gap); fillers default to an
-- online (offline = FALSE) state. CPUs that are never offline carry no cpuhp
-- slices at all (so INTERVAL FILL cannot introduce their lane); they get a
-- single whole-trace online slice from the device policy map.
CREATE PERFETTO PIPELINE _gapless_hotplug_slices MATERIALIZED AS
SUBPIPELINE never_offline AS (
  -- Creates a single online slice spanning the entire trace for CPUs that are
  -- never offline. This is needed for interval_intersect() to not delete
  -- undefined time periods
  FROM _dev_cpu_policy_map
  |> WHERE NOT (cpu IN (SELECT cpu FROM _cpu_hotplug_offline))
  |> SELECT trace_start() AS ts, trace_dur() AS dur, cpu, FALSE AS offline
)
FROM _cpu_hotplug_offline
|> SELECT ts, dur, cpu, is_different_cpu AS offline
|> INTERVAL FILL WITHIN trace_bounds PER cpu
|> SELECT ts, dur, cpu, coalesce(offline, FALSE) AS offline
|> UNION ALL (FROM never_offline)
|> ORDER BY cpu, ts;

-- Nominal suspend table with gapless suspended state.
CREATE PERFETTO PIPELINE _gapless_suspend_slices MATERIALIZED AS
FROM android_suspend_state
|> SELECT ts, dur, iif(power_state = 'suspended', TRUE, FALSE) AS suspended;
