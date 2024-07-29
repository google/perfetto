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

INCLUDE PERFETTO MODULE linux.cpu.idle;

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
WITH grouped AS (
  SELECT
    cpu,
    (idle + 1) AS state,
    COUNT(idle) AS count,
    SUM(dur) AS dur,
    SUM(dur) / COUNT(idle) AS avg_dur
  FROM cpu_idle_counters c
  WHERE c.idle >= 0
  GROUP BY c.cpu, c.idle
),
total AS (
  SELECT cpu, SUM(dur) AS dur
  FROM cpu_idle_counters
  GROUP BY cpu
)
SELECT
  g.*,
  g.dur * 100.0 / t.dur AS idle_percent
FROM grouped g
JOIN total t USING (cpu);
