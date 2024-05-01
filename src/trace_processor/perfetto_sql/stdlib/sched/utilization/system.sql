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

INCLUDE PERFETTO MODULE sched.utilization.general;
INCLUDE PERFETTO MODULE time.conversion;

-- The purpose of this module is to provide high level aggregates of system
-- utilization, akin to /proc/stat results.

-- Returns a table of system utilization per given period.
-- Utilization is calculated as sum of average utilization of each CPU in each
-- period, which is defined as a multiply of |interval|. For this reason
-- first and last period might have lower then real utilization.
CREATE PERFETTO FUNCTION sched_utilization_per_period(
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
FROM _sched_avg_utilization_per_period!(
  $interval,
  (SELECT * FROM sched WHERE utid != 0)
);

-- Table with system utilization per second.
-- Utilization is calculated by sum of average utilization of each CPU every
-- second. For this reason first and last second might have lower then real
-- utilization.
CREATE PERFETTO TABLE sched_utilization_per_second(
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
FROM sched_utilization_per_period(time_from_s(1));