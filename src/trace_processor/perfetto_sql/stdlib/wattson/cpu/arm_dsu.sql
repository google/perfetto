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

INCLUDE PERFETTO MODULE linux.devfreq;

INCLUDE PERFETTO MODULE wattson.curves.utils;

INCLUDE PERFETTO MODULE wattson.device_infos;

INCLUDE PERFETTO MODULE wattson.utils;

-- Converts event counter from count to rate (num of accesses per ns).
CREATE PERFETTO FUNCTION _get_rate(event STRING)
RETURNS TABLE(ts TIMESTAMP, dur DURATION, access_rate LONG)
AS
SELECT *
FROM (
  SELECT
    ts,
    lead(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts AS dur,
    -- Rate of event accesses in a section (i.e. count / dur).
    -- If the event name ends in '_cpu0', then the counter is "counts per period".
    -- If the event name does not end in '_cpu0', then the counter is monotonic.
    iif(
      $event GLOB "*_cpu0",
      value,
      lead(value) OVER (PARTITION BY track_id ORDER BY ts) - value
    )
    * 1.0
    / (lead(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts) AS access_rate
  FROM counter AS c
  JOIN counter_track AS t ON c.track_id = t.id
  WHERE
    t.name = $event
)
WHERE
  dur > 0;

-- The rate of L3 misses for each time slice based on the ARM DSU PMU counter's
-- bus_access event. Units will be in number of L3 misses per ns. The number of
-- accesses in a given duration can be calculated by multiplying the appropriate
-- rate with the time in the window of interest.
CREATE PERFETTO TABLE _arm_l3_miss_rate AS
WITH
  base AS (
    SELECT ts, dur, access_rate AS l3_miss_rate
    FROM _get_rate("arm_dsu_0/bus_access/_cpu0")
    UNION ALL
    SELECT ts, dur, access_rate AS l3_miss_rate
    FROM _get_rate("arm_dsu_0-bus_access")
    WHERE
      NOT EXISTS (
        SELECT 1 FROM counter_track WHERE name = "arm_dsu_0/bus_access/_cpu0"
      )
  )
SELECT trace_start() AS ts, min(ts) - trace_start() AS dur, 0 AS l3_miss_rate
FROM base
HAVING
  min(ts) > trace_start()
UNION ALL
SELECT ts, dur, l3_miss_rate FROM base
UNION ALL
SELECT trace_start(), trace_dur(), 0 WHERE NOT EXISTS (SELECT 1 FROM base);

-- The rate of L3 accesses for each time slice based on the ARM DSU PMU
-- counter's l3d_cache event. Units will be in number of DDR accesses per ns.
-- The number of accesses in a given duration can be calculated by multiplying
-- the appropriate rate with the time in the window of interest.
CREATE PERFETTO TABLE _arm_l3_hit_rate AS
WITH
  base AS (
    SELECT ts, dur, access_rate AS l3_hit_rate
    FROM _get_rate("arm_dsu_0/l3d_cache/_cpu0")
    UNION ALL
    SELECT ts, dur, access_rate AS l3_hit_rate
    FROM _get_rate("arm_dsu_0-l3d_cache")
    WHERE
      NOT EXISTS (
        SELECT 1 FROM counter_track WHERE name = "arm_dsu_0/l3d_cache/_cpu0"
      )
  )
SELECT trace_start() AS ts, min(ts) - trace_start() AS dur, 0 AS l3_hit_rate
FROM base
HAVING
  min(ts) > trace_start()
UNION ALL
SELECT ts, dur, l3_hit_rate FROM base
UNION ALL
SELECT trace_start(), trace_dur(), 0 WHERE NOT EXISTS (SELECT 1 FROM base);

-- Combine L3 hit and miss rates into a single table.
CREATE PERFETTO TABLE _arm_l3_rates AS
INTERVAL INTERSECTION OF (_arm_l3_miss_rate AS miss, _arm_l3_hit_rate AS hit)
|> SELECT ts, dur, miss.l3_miss_rate, hit.l3_hit_rate;

-- Gapless DSU frequency slices from trace_start() to trace_end().
-- Fills any leading gap prior to the first devfreq event (or the entire trace
-- when no devfreq events are present, e.g. on VMs or when DSU frequency is
-- pinned) with the device's minimum DSU frequency.
CREATE PERFETTO TABLE _wattson_dsu_frequency AS
FROM (
  SELECT ts, dur, dsu_freq
  FROM _use_devfreq_for_calc
  CROSS JOIN linux_devfreq_dsu_counter
)
|> INTERVAL FILL GAPS WITH (
  SELECT
    trace_start() AS ts,
    trace_dur() AS dur,
    iif(
      EXISTS (SELECT 1 FROM _use_devfreq_for_calc),
      coalesce(
        (SELECT min(dsu_freq) FROM _filtered_curves_dsu_1d),
        (
          SELECT min(dep_freq)
          FROM _cpu_w_dependency_default_vote
          WHERE
            dep_policy = _dsu_dep!()
        ),
        0
      ),
      0
    ) AS dsu_freq
);
