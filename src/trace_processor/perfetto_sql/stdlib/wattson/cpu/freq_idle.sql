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

INCLUDE PERFETTO MODULE intervals.intersect;

INCLUDE PERFETTO MODULE wattson.cpu.freq;

INCLUDE PERFETTO MODULE wattson.cpu.hotplug;

INCLUDE PERFETTO MODULE wattson.cpu.idle;

INCLUDE PERFETTO MODULE wattson.device_infos;

INCLUDE PERFETTO MODULE wattson.utils;

-- Wattson estimation is valid from when first CPU0 frequency appears
CREATE PERFETTO TABLE _valid_window AS
WITH
  window_start AS (
    SELECT ts AS start_ts
    FROM _adjusted_cpu_freq
    WHERE
      cpu = 0
      AND freq IS NOT NULL
    ORDER BY
      ts
    LIMIT 1
  )
SELECT start_ts AS ts, trace_end() - start_ts AS dur, cpu
FROM window_start
CROSS JOIN _dev_cpu_policy_map;

-- Start matching CPUs with 1D curves based on combination of freq and idle.
-- Passing freq and idle directly as id into _interval_intersect! eliminates
-- rowid table joins inside _idle_freq_materialized, and carrying state_id
-- through _stats_cpu0..7 eliminates 10 table joins in _w_cpu_slices.
CREATE PERFETTO TABLE _idle_freq_materialized AS
WITH
  params AS MATERIALIZED (
    SELECT
      (SELECT ts FROM _valid_window LIMIT 1) AS start_ts,
      (SELECT _encode_idle!(idle) FROM _deepest_idle) AS deepest_id,
      EXISTS (SELECT 1 FROM _cpu_hotplug_offline WHERE is_different_cpu) AS has_hotplug
  )
-- Fast path: zero-join 2-way intersect when no CPU hotplug offline events exist
SELECT
  max(ii.ts, p.start_ts) AS ts,
  ii.ts + ii.dur - max(ii.ts, p.start_ts) AS dur,
  ii.cpu,
  _pack_cpu_state!(
    ii.cpu,
    ii.id_0,
    iif(ii.id_1 = p.deepest_id, _deepest_idle_id!(), ii.id_1)
  ) AS state_id
FROM params AS p
CROSS JOIN _interval_intersect!(
  (
    (SELECT coalesce(freq, 0) AS id, ts, dur, cpu FROM _adjusted_cpu_freq),
    (SELECT coalesce(_encode_idle!(idle), _null_idle_id!()) AS id, ts, dur, cpu FROM _adjusted_deep_idle)
  ),
  (cpu)
) AS ii
WHERE
  NOT p.has_hotplug
  AND ii.ts + ii.dur > p.start_ts
UNION ALL
-- Zero-join 3-way intersect when CPU hotplug offline events exist
SELECT
  max(ii.ts, p.start_ts) AS ts,
  ii.ts + ii.dur - max(ii.ts, p.start_ts) AS dur,
  ii.cpu,
  _pack_cpu_state!(
    ii.cpu,
    ii.id_0,
    CASE
      WHEN ii.id_2 THEN _offline_idle_id!()
      WHEN ii.id_1 = p.deepest_id THEN _deepest_idle_id!()
      ELSE ii.id_1
    END
  ) AS state_id
FROM params AS p
CROSS JOIN _interval_intersect!(
  (
    (SELECT coalesce(freq, 0) AS id, ts, dur, cpu FROM _adjusted_cpu_freq),
    (SELECT coalesce(_encode_idle!(idle), _null_idle_id!()) AS id, ts, dur, cpu FROM _adjusted_deep_idle),
    (SELECT offline AS id, ts, dur, cpu FROM _gapless_hotplug_slices)
  ),
  (cpu)
) AS ii
WHERE
  p.has_hotplug
  AND ii.ts + ii.dur > p.start_ts
UNION ALL
-- Fallback dummy slice for CPUs 0..7 not present on this device
SELECT
  trace_start() AS ts,
  trace_dur() AS dur,
  missing.cpu,
  _pack_cpu_state!(missing.cpu, 0, _offline_idle_id!()) AS state_id
FROM (
  SELECT 0 AS cpu
  UNION ALL
  SELECT 1
  UNION ALL
  SELECT 2
  UNION ALL
  SELECT 3
  UNION ALL
  SELECT 4
  UNION ALL
  SELECT 5
  UNION ALL
  SELECT 6
  UNION ALL
  SELECT 7
) AS missing
WHERE
  NOT EXISTS (SELECT 1 FROM _dev_cpu_policy_map AS m WHERE m.cpu = missing.cpu);
