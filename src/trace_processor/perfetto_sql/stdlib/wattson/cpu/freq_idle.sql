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

-- Inputs for _idle_freq_intersect that are fixed for the whole trace.
CREATE PERFETTO TABLE _idle_freq_params AS
SELECT
  (SELECT ts FROM _valid_window LIMIT 1) AS start_ts,
  (SELECT _encode_idle!(idle) FROM _deepest_idle) AS deepest_id,
  EXISTS (SELECT 1 FROM _cpu_hotplug_offline WHERE is_different_cpu) AS has_hotplug;

-- CPUs 0..7 not present on this device.
CREATE PERFETTO TABLE _idle_freq_missing_cpus AS
SELECT missing.cpu
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

-- Per CPU intersection of frequency, idle and hotplug state.
--
-- Frequency is cut to start at the valid window, which cuts the result too.
-- Without hotplug offline events, each CPU gets one online slice that spans
-- the trace, so the hotplug operand adds no boundaries. Each missing CPU gets
-- one offline slice that spans the trace.
CREATE PERFETTO TABLE _idle_freq_intersect AS
INTERVAL INTERSECTION OF (
  (
    SELECT
      max(f.ts, p.start_ts) AS ts,
      f.ts + f.dur - max(f.ts, p.start_ts) AS dur,
      f.cpu,
      coalesce(f.freq, 0) AS freq
    FROM _adjusted_cpu_freq AS f, _idle_freq_params AS p
    WHERE
      f.ts + f.dur > p.start_ts
    UNION ALL
    SELECT trace_start() AS ts, trace_dur() AS dur, cpu, 0 AS freq
    FROM _idle_freq_missing_cpus
  ) AS f,
  (
    SELECT
      i.ts,
      i.dur,
      i.cpu,
      iif(
        coalesce(_encode_idle!(i.idle), _null_idle_id!()) = p.deepest_id,
        _deepest_idle_id!(),
        coalesce(_encode_idle!(i.idle), _null_idle_id!())
      ) AS idle_id
    FROM _adjusted_deep_idle AS i, _idle_freq_params AS p
    WHERE
      NOT (i.cpu IN (SELECT cpu FROM _idle_freq_missing_cpus))
    UNION ALL
    SELECT
      trace_start() AS ts,
      trace_dur() AS dur,
      cpu,
      _offline_idle_id!() AS idle_id
    FROM _idle_freq_missing_cpus
  ) AS i,
  (
    SELECT h.ts, h.dur, h.cpu, h.offline
    FROM _gapless_hotplug_slices AS h, _idle_freq_params AS p
    WHERE
      p.has_hotplug
      AND NOT (h.cpu IN (SELECT cpu FROM _idle_freq_missing_cpus))
    UNION ALL
    SELECT trace_start() AS ts, trace_dur() AS dur, m.cpu, FALSE AS offline
    FROM _dev_cpu_policy_map AS m, _idle_freq_params AS p
    WHERE
      NOT p.has_hotplug
    UNION ALL
    SELECT trace_start() AS ts, trace_dur() AS dur, cpu, FALSE AS offline
    FROM _idle_freq_missing_cpus
  ) AS h
) PER cpu
|> SELECT ts, dur, cpu, f.freq, i.idle_id, h.offline;

-- Start matching CPUs with 1D curves based on combination of freq and idle.
-- Carrying state_id through _w_cpu_slices eliminates 10 table joins.
CREATE PERFETTO VIEW _idle_freq_materialized AS
SELECT
  ts,
  dur,
  cpu,
  _pack_cpu_state!(cpu, freq, iif(offline, _offline_idle_id!(), idle_id)) AS state_id
FROM _idle_freq_intersect;
