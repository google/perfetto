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

INCLUDE PERFETTO MODULE wattson.cpu_freq;
INCLUDE PERFETTO MODULE wattson.cpu_idle;
INCLUDE PERFETTO MODULE wattson.curves.utils;

-- Combines idle and freq tables of all CPUs to create system state.
CREATE VIRTUAL TABLE _idle_freq
USING
  SPAN_OUTER_JOIN(
    _adjusted_cpu_freq partitioned cpu, _adjusted_deep_idle partitioned cpu
  );

-- Add extra column indicating that frequency info are present
CREATE PERFETTO TABLE _valid_window
AS
WITH window_start AS (
  SELECT ts as start_ts
  FROM _idle_freq
  WHERE cpu = 0 and freq GLOB '*[0-9]*'
  ORDER BY ts ASC
  LIMIT 1
),
window_end AS (
  SELECT ts + dur as end_ts
  FROM cpu_frequency_counters
  ORDER by ts DESC
  LIMIT 1
)
SELECT
  start_ts as ts,
  end_ts - start_ts as dur
FROM window_start, window_end;

CREATE VIRTUAL TABLE _idle_freq_filtered
USING
  SPAN_JOIN(_valid_window, _idle_freq);

