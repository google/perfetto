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

-- This table aggregates the total scheduling duration for each CPU.
-- The durations are derived from the 'dur' column of scheduling slices ('sched' table entries).
--
-- Data sources that need to be enabled: linux.ftrace
CREATE PERFETTO TABLE cpu_sched_time (
  -- CPU id
  cpu LONG,
  -- Unique CPU id
  ucpu LONG,
  -- CPU sched duration: the sum of 'dur' (duration) for all scheduling slices ('sched' table entries)
  -- on this CPU within the trace
  cpu_sched_dur_ns LONG
) AS
SELECT
  cpu,
  ucpu,
  sum(dur) AS cpu_sched_dur_ns
FROM sched
GROUP BY
  cpu,
  ucpu;
