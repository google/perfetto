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

INCLUDE PERFETTO MODULE linux.cpu.utilization.sched;

CREATE PERFETTO TABLE _kswapd_sched_time AS
SELECT
  -- CPU id
  cpu,
  -- Unique CPU id
  ucpu,
  -- 'kswapd0' sched duration: the sum of 'dur' (duration) for scheduling slices ('sched' table entries)
  -- where the 'kswapd0' thread was present on this specific CPU within the trace.
  sum(dur) AS kswapd_sched_dur_ns
FROM sched
JOIN thread
  USING (utid)
WHERE
  thread.name = 'kswapd0'
GROUP BY
  cpu,
  ucpu;

-- This view quantifies the CPU time consumed by the 'kswapd0' thread on each CPU.
-- The durations provided are derived from the 'dur' column of scheduling slices
-- within the 'sched' table. It presents the total accumulated duration for each
-- CPU (from 'sched' entries) and the portion of that duration attributed
-- to 'kswapd0', expressed both in nanoseconds and as a percentage.
--
-- Data sources that need to be enabled: linux.ftrace
CREATE PERFETTO VIEW linux_kswapd_cpu_breakdown (
  -- CPU id
  cpu LONG,
  -- Unique CPU id
  ucpu LONG,
  -- CPU sched duration: the sum of 'dur' (duration) for all scheduling slices ('sched' table entries)
  -- on this CPU within the trace.
  cpu_sched_dur_ns LONG,
  -- 'kswapd0' sched duration: the sum of 'dur' (duration) for scheduling slices ('sched' table entries)
  -- where the 'kswapd0' thread was present on this specific CPU within the trace.
  kswapd_sched_dur_ns LONG,
  -- The percentage of time on a CPU attributed to 'kswapd0', relative to
  -- the total CPU duration, derived from 'sched' table data.
  -- Calculated as (kswapd_sched_dur_ns / cpu_sched_dur_ns) * 100.
  -- Returns 0.0 if 'cpu_sched_dur_ns' is zero.
  kswapd_cpu_dur_percentage DOUBLE
) AS
SELECT
  ct.cpu,
  ucpu,
  cpu_sched_dur_ns,
  coalesce(kswapd_sched_dur_ns, 0) AS kswapd_sched_dur_ns,
  iif(
    cpu_sched_dur_ns > 0,
    round((
      coalesce(kswapd_sched_dur_ns, 0) * 100.0
    ) / cpu_sched_dur_ns, 2),
    0.0
  ) AS kswapd_cpu_dur_percentage
FROM cpu_sched_time AS ct
LEFT JOIN _kswapd_sched_time
  USING (ucpu);
