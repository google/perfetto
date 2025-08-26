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

INCLUDE PERFETTO MODULE linux.cpu.sched_breakdown;

-- Helper table that calculates the total duration 'kswapd0' kernel thread
-- was running for each CPU within the trace.
CREATE PERFETTO TABLE _linux_kswapd_sched_time AS
SELECT
  -- CPU id
  cpu,
  -- Unique CPU id
  ucpu,
  -- 'kswapd0' sched duration: sum of 'dur' (duration) for 'kswapd0' sched slices ('sched' table entries),
  -- for each CPU within the trace.
  sum(dur) AS kswapd_sched_dur_ns
FROM sched
JOIN thread
  USING (utid)
WHERE
  thread.name = 'kswapd0'
GROUP BY
  cpu,
  ucpu;

-- This table quantifies the CPU time consumed by the 'kswapd0' kernel thread on each CPU core,
-- relative to the total time the CPU running *non-idle* tasks and the power state was 'awake'(not suspended).
--
-- This metric relies on the 'cpu_sched_awake_time' table from module linux.cpu.sched_breakdown, which intersects
-- sched slices with 'awake' power state slices from 'android_suspend_state' table.
--
-- For the 'kswapd0' thread, there is no need to intersect with power state slices, as it is almost
-- certain that power state is 'awake' when 'kswapd0' is running.
--
-- Data sources that need to be enabled: linux.ftrace
CREATE PERFETTO TABLE linux_kswapd_cpu_breakdown (
  -- CPU id
  cpu LONG,
  -- Unique CPU id
  ucpu LONG,
  -- CPU sched duration while 'awake': sum of 'dur' (duration) where sched slices intersect with 'awake' power state slices,
  -- for each CPU within the trace.
  cpu_sched_awake_dur_ns LONG,
  -- 'kswapd0' sched duration: sum of 'dur' (duration) for 'kswapd0' sched slices ('sched' table entries),
  -- for each CPU within the trace.
  kswapd_sched_dur_ns LONG,
  -- The percentage of time on a CPU attributed to 'kswapd0'.
  -- Calculated as (kswapd_sched_dur_ns / cpu_sched_awake_dur_ns) * 100.
  -- Returns 0.0 if 'cpu_sched_awake_dur_ns' is zero.
  -- Returns values in the [0.0, 100.0] range.
  kswapd_cpu_awake_dur_percentage DOUBLE
) AS
SELECT
  ct.cpu,
  ucpu,
  cpu_sched_awake_dur_ns,
  coalesce(kswapd_sched_dur_ns, 0) AS kswapd_sched_dur_ns,
  iif(
    cpu_sched_awake_dur_ns > 0,
    round((
      coalesce(kswapd_sched_dur_ns, 0) * 100.0
    ) / cpu_sched_awake_dur_ns, 2),
    0.0
  ) AS kswapd_cpu_awake_dur_percentage
FROM cpu_sched_awake_time AS ct
LEFT JOIN _linux_kswapd_sched_time
  USING (ucpu);

-- This table calculates a system-wide metric of 'kswapd0' utilization.
--
-- The utilization is calculated as total time spent by 'kswapd0' thread
-- expressed as percentage of the total trace time when there was
-- any CPU activity (i.e. running tasks and not in 'suspended' power state).
--
-- Data sources that need to be enabled: linux.ftrace
CREATE PERFETTO TABLE linux_kswapd_utilization (
  -- The percentage of time spent by 'kswapd0' thread relative to total trace time
  -- when there was any CPU activity (i.e. running tasks and not in 'suspended' power state).
  -- Returns values in the [0.0, 100.0] range.
  kswapd_utilization_percentage DOUBLE
) AS
SELECT
  (
    SELECT
      sum(s.dur)
    FROM sched AS s
    JOIN thread AS t
      USING (utid)
    WHERE
      t.name = 'kswapd0' AND s.dur != -1
  ) * 100.0 / (
    -- TO_MONOTONIC(ts) gives the timestamp using a monotonic clock, which by definition
    -- does not include the time when the device power state is 'suspended'.
    SELECT
      max(to_monotonic(ts) + dur) - min(to_monotonic(ts))
    FROM sched
    WHERE
      dur != -1
  ) AS kswapd_utilization_percentage;
