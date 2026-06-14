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
CREATE PERFETTO PIPELINE cpu_idle_stats(
  -- CPU core number.
  cpu LONG,
  -- CPU idle state (C-states).
  state LONG,
  -- The count of entering idle state.
  count LONG,
  -- Total CPU core idle state duration.
  dur DURATION,
  -- Average CPU core idle state duration.
  avg_dur DURATION,
  -- Idle state percentage of non suspend time (C-states + P-states).
  idle_percent DOUBLE
)
MATERIALIZED AS
SUBPIPELINE total AS (
  FROM cpu_idle_counters
  |> AGGREGATE sum(dur) AS dur GROUP BY cpu
)
FROM cpu_idle_counters AS c
|> WHERE c.idle >= 0
|> AGGREGATE
     count(idle) AS count,
     sum(dur) AS dur,
     sum(dur) / count(idle) AS avg_dur
   GROUP BY c.cpu AS cpu, c.idle + 1 AS state
|> JOIN total AS t USING (cpu)
|> SELECT
     cpu,
     state,
     count,
     dur,
     avg_dur,
     dur * 100.0 / t.dur AS idle_percent;
