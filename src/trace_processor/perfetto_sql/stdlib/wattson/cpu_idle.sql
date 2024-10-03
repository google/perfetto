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

INCLUDE PERFETTO MODULE counters.intervals;
INCLUDE PERFETTO MODULE wattson.device_infos;

-- Get the corresponding deep idle time offset based on device and CPU.
CREATE PERFETTO VIEW _filtered_deep_idle_offsets AS
SELECT cpu, offset_ns
FROM _device_cpu_deep_idle_offsets as offsets
JOIN _wattson_device as device
ON offsets.device = device.name;

-- Adjust duration of active portion to be slightly longer to account for
-- overhead cost of transitioning out of deep idle. This is done because the
-- device is active and consumes power for longer than the logs actually report.
CREATE PERFETTO TABLE _adjusted_deep_idle AS
WITH
  idle_prev AS (
    SELECT
      ts,
      LAG(ts, 1, trace_start()) OVER (PARTITION BY cpu ORDER by ts) as prev_ts,
      value AS idle,
      cli.value - cli.delta_value AS idle_prev,
      cct.cpu
    -- Same as cpu_idle_counters, but extracts some additional info that isn't
    -- nominally present in cpu_idle_counters, such that the already calculated
    -- lag values are reused instead of recomputed
    FROM counter_leading_intervals!((
      SELECT c.*
      FROM counter c
      JOIN cpu_counter_track cct ON cct.id = c.track_id AND cct.name = 'cpuidle'
    )) AS cli
    JOIN cpu_counter_track AS cct ON cli.track_id = cct.id
  ),
  -- Adjusted ts if applicable, which makes the current active state longer if
  -- it is coming from an idle exit.
  idle_mod AS (
    SELECT
      IIF(
        idle_prev = 1 AND idle = 4294967295,
        -- extend ts backwards by offset_ns at most up to prev_ts
        MAX(ts - offset_ns, prev_ts),
        ts
      ) as ts,
      cpu,
      idle
    FROM idle_prev
    JOIN _filtered_deep_idle_offsets USING (cpu)
  ),
  _cpu_idle AS (
    SELECT
      ts,
      LEAD(ts, 1, trace_end()) OVER (PARTITION BY cpu ORDER by ts) - ts as dur,
      cpu,
      cast_int!(IIF(idle = 4294967295, -1, idle)) AS idle
    FROM idle_mod
  ),
  -- Get first idle transition per CPU
  first_cpu_idle_slices AS (
    SELECT ts, cpu FROM _cpu_idle
    GROUP BY cpu
    ORDER by ts ASC
  )
-- Prepend NULL slices up to first idle events on a per CPU basis
SELECT
  -- Construct slices from first cpu ts up to first freq event for each cpu
  trace_start() as ts,
  first_slices.ts - trace_start() as dur,
  first_slices.cpu,
  NULL as idle
FROM first_cpu_idle_slices as first_slices
WHERE dur > 0
UNION ALL
SELECT
  ts,
  dur,
  cpu,
  idle
FROM _cpu_idle
-- Some durations are 0 post-adjustment and won't work with interval intersect
WHERE dur > 0;
