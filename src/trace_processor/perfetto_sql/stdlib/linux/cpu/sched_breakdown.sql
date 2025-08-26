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

INCLUDE PERFETTO MODULE android.suspend;

INCLUDE PERFETTO MODULE intervals.intersect;

-- This table aggregates the total duration each CPU core spent running *non-idle* tasks
-- ONLY during periods when the device power state was 'awake' (i.e., not suspended).
--
-- The durations are calculated by intersecting the time intervals of:
--   1. Non-idle task scheduling slices from the 'sched' table (utid != 0).
--   2. 'awake' power state slices from the 'android_suspend_state' table.
--
-- Data sources that need to be enabled: linux.ftrace
CREATE PERFETTO TABLE cpu_sched_awake_time (
  -- CPU id
  cpu LONG,
  -- Unique CPU id
  ucpu LONG,
  -- CPU sched duration while 'awake': sum of 'dur' (duration) where sched slices intersect with 'awake' power state slices,
  -- for each CPU within the trace.
  cpu_sched_awake_dur_ns LONG
) AS
WITH
  cpu_sched_valid AS (
    SELECT
      cpu,
      ucpu,
      id,
      ts,
      dur
    FROM sched
    WHERE
      dur > 0 AND utid != 0
  ),
  awake_slices_with_id AS (
    SELECT
      row_number() OVER (ORDER BY ts) AS id,
      ts,
      dur
    FROM android_suspend_state
    WHERE
      power_state = 'awake' AND dur > 0
    ORDER BY
      id
  )
SELECT
  cpu,
  ucpu,
  sum(overlap.dur) AS cpu_sched_awake_dur_ns
FROM _interval_intersect!(
  (cpu_sched_valid, awake_slices_with_id),()
) AS overlap
JOIN cpu_sched_valid AS cs
  ON overlap.id_0 = cs.id
GROUP BY
  cpu,
  ucpu;
