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

INCLUDE PERFETTO MODULE linux.cpu.utilization.general;
INCLUDE PERFETTO MODULE time.conversion;

-- The purpose of this module is to provide high level aggregates of system
-- utilization, akin to /proc/stat results.

-- Returns a table of system utilization per given period.
-- Utilization is calculated as sum of average utilization of each CPU in each
-- period, which is defined as a multiply of |interval|. For this reason
-- first and last period might have lower then real utilization.
CREATE PERFETTO FUNCTION cpu_utilization_per_period(
  -- Length of the period on which utilization should be averaged.
  interval INT)
RETURNS TABLE (
  -- Timestamp of start of a second.
  ts INT,
  -- Sum of average utilization over period.
  -- Note: as the data is normalized, the values will be in the
  -- [0, 1] range.
  utilization DOUBLE,
  -- Sum of average utilization over all CPUs over period.
  -- Note: as the data is unnormalized, the values will be in the
  -- [0, cpu_count] range.
  unnormalized_utilization DOUBLE
) AS
SELECT *
FROM _cpu_avg_utilization_per_period!(
  $interval,
  (SELECT * FROM sched WHERE utid != 0)
);

-- Table with system utilization per second.
-- Utilization is calculated by sum of average utilization of each CPU every
-- second. For this reason first and last second might have lower then real
-- utilization.
CREATE PERFETTO TABLE cpu_utilization_per_second(
  -- Timestamp of start of a second.
  ts INT,
  -- Sum of average utilization over period.
  -- Note: as the data is normalized, the values will be in the
  -- [0, 1] range.
  utilization DOUBLE,
  -- Sum of average utilization over all CPUs over period.
  -- Note: as the data is unnormalized, the values will be in the
  -- [0, cpu_count] range.
  unnormalized_utilization DOUBLE
) AS
SELECT
  ts,
  utilization,
  unnormalized_utilization
FROM cpu_utilization_per_period(time_from_s(1));

-- Aggregated CPU statistics for runtime of each thread on a CPU.
CREATE PERFETTO TABLE _cpu_cycles_raw(
  -- The id of CPU
  cpu INT,
  -- Unique thread id
  utid INT,
  -- Sum of CPU millicycles
  millicycles INT,
  -- Sum of CPU megacycles
  megacycles INT,
  -- Total runtime duration
  runtime INT,
  -- Minimum CPU frequency in kHz
  min_freq INT,
  -- Maximum CPU frequency in kHz
  max_freq INT,
  -- Average CPU frequency in kHz
  avg_freq INT
) AS
SELECT
  cpu,
  utid,
  -- We divide by 1e3 here as dur is in ns and freq in khz. In total
  -- this means we need to divide the duration by 1e9 and multiply the
  -- frequency by 1e3 then multiply again by 1e3 to get millicycles
  -- i.e. divide by 1e3 in total.
  -- We use millicycles as we want to preserve this level of precision
  -- for future calculations.
  cast_int!(SUM(dur * freq) / 1000) AS millicycles,
  cast_int!(SUM(dur * freq) / 1000 / 1e9) AS megacycles,
  SUM(dur) AS runtime,
  MIN(freq) AS min_freq,
  MAX(freq) AS max_freq,
  -- We choose to work in micros space in both the numerator and
  -- denominator as this gives us good enough precision without risking
  -- overflows.
  cast_int!(SUM((dur * freq) / 1000) / SUM(dur / 1000)) AS avg_freq
FROM _cpu_freq_per_thread
GROUP BY utid, cpu;

-- Aggregated CPU statistics for each CPU.
CREATE PERFETTO TABLE cpu_cycles_per_cpu(
  -- The id of CPU
  cpu INT,
  -- Sum of CPU millicycles
  millicycles INT,
  -- Sum of CPU megacycles
  megacycles INT,
  -- Total runtime of all threads running on CPU
  runtime INT,
  -- Minimum CPU frequency in kHz
  min_freq INT,
  -- Maximum CPU frequency in kHz
  max_freq INT,
  -- Average CPU frequency in kHz
  avg_freq INT
) AS
SELECT
  cpu,
  cast_int!(SUM(dur * freq) / 1000) AS millicycles,
  cast_int!(SUM(dur * freq) / 1000 / 1e9) AS megacycles,
  SUM(dur) AS runtime,
  MIN(freq) AS min_freq,
  MAX(freq) AS max_freq,
  cast_int!(SUM((dur * freq) / 1000) / SUM(dur / 1000)) AS avg_freq
FROM _cpu_freq_per_thread
GROUP BY cpu;