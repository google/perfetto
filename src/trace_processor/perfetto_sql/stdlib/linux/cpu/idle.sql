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
INCLUDE PERFETTO MODULE linux.cpu.frequency;

-- Counter information for each idle state change for each CPU. Finds each time
-- region where a CPU idle state is constant.
CREATE PERFETTO TABLE cpu_idle_counters(
  -- Counter id.
  id INT,
  -- Joinable with 'counter_track.id'.
  track_id INT,
  -- Starting timestamp of the counter.
  ts LONG,
  -- Duration in which the counter is contant and idle state doesn't change.
  dur INT,
  -- Idle state of the CPU that corresponds to this counter. An idle state of -1
  -- is defined to be active state for the CPU, and the larger the integer, the
  -- deeper the idle state of the CPU. NULL if not found or undefined.
  idle INT,
  -- CPU that corresponds to this counter.
  cpu INT
)
AS
SELECT
  count_w_dur.id,
  count_w_dur.track_id,
  count_w_dur.ts,
  count_w_dur.dur,
  cast_int!(IIF(count_w_dur.value = 4294967295, -1, count_w_dur.value)) AS idle,
  cct.cpu
FROM
counter_leading_intervals!((
  SELECT c.*
  FROM counter c
  JOIN cpu_counter_track cct
  ON cct.id = c.track_id and cct.name = 'cpuidle'
)) count_w_dur
JOIN cpu_counter_track cct
ON track_id = cct.id;

CREATE PERFETTO VIEW _freq_counters_for_sp_jn AS
SELECT ts, dur, cpu
FROM cpu_frequency_counters;

CREATE PERFETTO VIEW _idle_counters_for_sp_jn AS
SELECT ts, dur, cpu, idle
FROM cpu_idle_counters;

-- Combined cpu freq & idle counter
CREATE VIRTUAL TABLE _freq_idle_counters
USING span_join(
  _freq_counters_for_sp_jn PARTITIONED cpu,
  _idle_counters_for_sp_jn PARTITIONED cpu
);

-- Aggregates cpu idle statistics per core.
CREATE PERFETTO TABLE cpu_idle_stats(
  -- CPU core number.
  cpu INT,
  -- CPU idle state (C-states).
  state INT,
  -- The count of entering idle state.
  count INT,
  -- Total CPU core idle state duration in nanoseconds.
  dur INT,
  -- Average CPU core idle state duration in nanoseconds.
  avg_dur INT,
  -- Idle state percentage of non suspend time (C-states + P-states).
  idle_percent FLOAT
)
AS
WITH
total AS (
  SELECT
    cpu,
    sum(dur) AS dur
  FROM _freq_idle_counters
  GROUP BY cpu
)
SELECT
  cpu,
  (idle + 1) AS state,
  COUNT(idle) AS count,
  SUM(dur) AS dur,
  SUM(dur) / COUNT(idle) AS avg_dur,
  SUM(dur) * 100.0 / (SELECT dur FROM total t WHERE t.cpu = ific.cpu) AS idle_percent
FROM _freq_idle_counters ific
WHERE idle >=0
GROUP BY cpu, idle;
