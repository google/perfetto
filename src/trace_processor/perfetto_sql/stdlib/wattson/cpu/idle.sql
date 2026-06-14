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

INCLUDE PERFETTO MODULE wattson.device_infos;

-- Get the corresponding deep idle time offset based on device and CPU.
CREATE PERFETTO PIPELINE _filtered_deep_idle_offsets AS
FROM _device_cpu_deep_idle_offsets AS offsets
|> JOIN _wattson_device AS device ON offsets.device = device.name
|> SELECT cpu, offset_ns;

-- Table that is empty if the actual cpuidle counters do not exist on this trace
CREATE PERFETTO PIPELINE _wattson_cpuidle_counters_exist AS
FROM cpu_counter_track
|> WHERE name = 'cpuidle'
|> SELECT id
|> LIMIT 1;

-- Create table that uses idle counters if present, otherwise extrapolates idle
-- states in a simplified way (only 2 states, active or idle) from the swapper
-- thread.
CREATE PERFETTO PIPELINE _unified_idle_state MATERIALIZED AS
-- If _wattson_cpuidle_counters_exist has rows, this returns empty, effectively
-- disabling the 'swapper_as_idle' branch efficiently.
SUBPIPELINE const_params AS (
  FROM _deepest_idle
  |> SELECT idle AS deepest_idle
  |> LIMIT 1
  |> WHERE NOT EXISTS (SELECT 1 FROM _wattson_cpuidle_counters_exist)
)
SUBPIPELINE continuous_idle_slices AS (
  -- Merge transition points if an idle slice exactly abuts an active state
  -- Transition to idle (using swapper as idle)
  FROM const_params AS p
  |> JOIN sched
  -- dur != 0 to handle unfinished slices
  |> WHERE utid IN (SELECT utid FROM thread WHERE is_idle) AND dur != 0
  |> SELECT ts, cpu, p.deepest_idle AS idle
  |> UNION ALL (
    -- Transition to active
    FROM const_params AS p
    |> JOIN sched
    -- dur > 0 to prevent ts + (-1)
    |> WHERE utid IN (SELECT utid FROM thread WHERE is_idle) AND dur > 0
    |> SELECT ts + dur AS ts, cpu, 4294967295 AS idle
  )
  |> AGGREGATE min(idle) AS idle GROUP BY cpu, ts
  |> SELECT
       ts,
       cpu,
       idle,
       lag(idle, 1, idle) OVER (PARTITION BY cpu ORDER BY ts) != idle AS transitioned
  |> WHERE transitioned
  |> SELECT ts, cpu, idle
)
FROM counter AS c
|> JOIN cpu_counter_track AS cct ON cct.id = c.track_id
|> WHERE cct.name = 'cpuidle'
|> SELECT
     c.ts,
     lag(c.ts, 1, trace_start()) OVER (
       PARTITION BY c.track_id ORDER BY c.ts, c.id
     ) AS prev_ts,
     cast_int!(c.value) AS idle,
     cast_int!(lag(c.value) OVER (PARTITION BY c.track_id ORDER BY c.ts, c.id)) AS idle_prev,
     cct.cpu
|> SELECT ts, prev_ts, idle, idle_prev, cpu
|> UNION ALL (
  FROM continuous_idle_slices
  |> SELECT
       ts,
       lag(ts, 1, trace_start()) OVER (PARTITION BY cpu ORDER BY ts) AS prev_ts,
       idle,
       lag(idle) OVER (PARTITION BY cpu ORDER BY ts) AS idle_prev,
       cpu
);

-- Adjust duration of active portion to be slightly longer to account for
-- overhead cost of transitioning out of deep idle. This is done because the
-- device is active and consumes power for longer than the logs actually report.
CREATE PERFETTO PIPELINE _adjusted_deep_idle MATERIALIZED AS
-- Adjusted ts if applicable, which makes the current active state longer if
-- it is coming from an idle exit.
SUBPIPELINE idle_mod AS (
  FROM _unified_idle_state
  |> JOIN _filtered_deep_idle_offsets USING (cpu)
  |> SELECT
       iif(
         idle_prev = 1 AND idle = 4294967295,
         -- extend ts backwards by offset_ns at most up to prev_ts
         max(ts - offset_ns, prev_ts),
         ts
       ) AS ts,
       cpu,
       idle
)
-- Use EITHER idle states as is OR device specific override of idle states.
-- The lead(ts) - ts duration construction per cpu is INTERVALS FROM EVENTS
-- closing the final interval at trace_end().
SUBPIPELINE _cpu_idle AS (
  -- Idle state calculations as is
  INTERVALS FROM EVENTS idle_mod PER cpu CLOSING LAST AT (trace_end())
  |> WHERE NOT EXISTS (SELECT 1 FROM _idle_state_map_override)
  |> SELECT ts, dur, cpu, cast_int!(IIF(idle = 4294967295, -1, idle)) AS idle
  |> UNION ALL (
    -- Device specific override of idle states
    INTERVALS FROM EVENTS idle_mod PER cpu CLOSING LAST AT (trace_end())
    |> JOIN _idle_state_map_override AS idle_map ON idle = idle_map.nominal_idle
    |> WHERE EXISTS (SELECT 1 FROM _idle_state_map_override)
    |> SELECT ts, dur, cpu, override_idle AS idle
  )
)
-- Get first idle transition per CPU
FROM _cpu_idle
|> AGGREGATE min(ts) AS ts GROUP BY cpu
|> FORK AS first_cpu_idle_slices
-- Prepend NULL slices up to first idle events on a per CPU basis.
-- Construct slices from first cpu ts up to first freq event for each cpu
|> SELECT
     trace_start() AS ts,
     ts - trace_start() AS dur,
     cpu,
     NULL AS idle
|> WHERE dur > 0
|> UNION ALL (
  FROM _cpu_idle
  |> WHERE dur > 0
  |> SELECT ts, dur, cpu, idle
)
-- Add empty cpu idle counters for CPUs that are physically present, but did not
-- have a single idle event register. The time region needs to be defined so
-- that interval_intersect doesn't remove the undefined time region.
|> UNION ALL (
  FROM _dev_cpu_policy_map
  |> WHERE NOT (cpu IN (SELECT cpu FROM first_cpu_idle_slices))
  |> SELECT trace_start() AS ts, trace_dur() AS dur, cpu, NULL AS idle
);
