--
-- Copyright 2025 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the 'License');
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an 'AS IS' BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

CREATE PERFETTO TABLE _android_cpu_total_time AS
SELECT
  -- CPU id
  cpu,
  -- Unique CPU id
  ucpu,
  -- The total accumulated duration, in nanoseconds, that the CPU was scheduled to run
  -- any task (both active and iddle states) within the trace.
  sum(dur) AS cpu_total_dur_ns
FROM sched
GROUP BY
  cpu,
  ucpu;

CREATE PERFETTO TABLE _android_kswapd_running_time AS
SELECT
  -- CPU id
  cpu,
  -- Unique CPU id
  ucpu,
  -- The total accumulated duration, in nanoseconds, that the 'kswapd0' thread
  -- was scheduled and running on this specific CPU within the trace.
  sum(dur) AS kswapd_running_dur_ns
FROM sched
JOIN thread
  USING (utid)
WHERE
  thread.name = 'kswapd0'
GROUP BY
  cpu,
  ucpu;

-- This view quantifies the CPU time consumed by the 'kswapd0' thread on each CPU.
-- It provides the total accumulated duration for which each CPU was scheduled to run any task
-- (both active and iddle states) and the portion of that duration
-- specifically attributed to 'kswapd0', expressed both in nanoseconds and as a percentage.
--
-- Data sources that need to be enabled: linux.ftrace
CREATE PERFETTO VIEW android_kswapd_cpu_breakdown (
  -- CPU id
  cpu LONG,
  -- Unique CPU id
  ucpu LONG,
  -- The total accumulated duration, in nanoseconds, that the CPU was scheduled to run
  -- any task (both active and iddle states) within the trace.
  cpu_total_dur_ns LONG,
  -- The total accumulated duration, in nanoseconds, that the 'kswapd0' thread
  -- was scheduled and running on this specific CPU within the trace.
  kswapd_running_dur_ns LONG,
  -- The percentage of time a CPU spent executing kswapd0, relative to
  -- the total time that CPU was scheduled (cpu_total_dur_ns).
  -- Calculated as (kswapd_running_dur_ns / cpu_total_dur_ns) * 100.
  -- Returns 0.0 if 'cpu_total_dur_ns' is zero.
  kswapd_cpu_time_percentage DOUBLE
) AS
SELECT
  cpu,
  ucpu,
  cpu_total_dur_ns,
  coalesce(kswapd_running_dur_ns, 0) AS kswapd_running_dur_ns,
  CASE
    WHEN cpu_total_dur_ns > 0
    THEN round((
      coalesce(kswapd_running_dur_ns, 0) * 100.0
    ) / cpu_total_dur_ns, 2)
    ELSE 0.0
  END AS kswapd_cpu_time_percentage
FROM _android_cpu_total_time
LEFT JOIN _android_kswapd_running_time
  USING (cpu, ucpu);
