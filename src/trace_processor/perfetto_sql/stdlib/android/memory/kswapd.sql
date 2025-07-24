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

-- Breakdown of kswapd duration on each cpu.
CREATE PERFETTO VIEW android_kswapd_cpu_breakdown (
  -- cpu
  cpu LONG,
  -- cpu duration
  cpu_dur_ns LONG,
  -- kswapd duration
  kswapd_dur_ns LONG,
  -- percentage of kswapd
  kswapd_percent DOUBLE
) AS
SELECT
  cpu,
  sum(dur) AS cpu_dur_ns,
  kswapd_dur_ns,
  CASE WHEN sum(dur) > 0 THEN round(100.0 * kswapd_dur_ns / sum(dur), 2) ELSE 0.0 END AS kswapd_percent
FROM sched
JOIN (
  SELECT
    cpu,
    sum(dur) AS kswapd_dur_ns
  FROM sched
  JOIN thread
    USING (utid)
  WHERE
    thread.name = 'kswapd0'
  GROUP BY
    cpu
) AS kd
  USING (cpu)
GROUP BY
  cpu;