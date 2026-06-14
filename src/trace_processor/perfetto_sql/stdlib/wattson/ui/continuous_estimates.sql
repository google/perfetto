--
-- Copyright 2025 The Android Open Source Project
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

INCLUDE PERFETTO MODULE wattson.estimates;

-- After ii, a single column will have the same value split up into different
-- slices. This macro recombines all the slices such that adjacent slices will
-- always have different values. This means less slices to process, and from the
-- UI perspective, the counter track will be displayed cleaner.
--
-- The input is already an interval stream (one lane), so consecutive slices
-- agreeing on the rail value are coalesced with INTERVAL MERGE CONSECUTIVE.
CREATE PERFETTO MACRO _get_continuous_estimates(rail ColumnName)
RETURNS TableOrSubquery
AS (
  FROM _system_state_mw
  |> SELECT ts, dur, $rail AS value
  |> INTERVAL MERGE CONSECUTIVE BY value AGGREGATE MIN(value) AS value
  |> RENAME value AS $rail
);

CREATE PERFETTO PIPELINE _system_state_cpu0_mw MATERIALIZED AS
FROM _get_continuous_estimates!(cpu0_mw) |> EXTEND 0 AS cpu;

CREATE PERFETTO PIPELINE _system_state_cpu1_mw MATERIALIZED AS
FROM _get_continuous_estimates!(cpu1_mw) |> EXTEND 1 AS cpu;

CREATE PERFETTO PIPELINE _system_state_cpu2_mw MATERIALIZED AS
FROM _get_continuous_estimates!(cpu2_mw) |> EXTEND 2 AS cpu;

CREATE PERFETTO PIPELINE _system_state_cpu3_mw MATERIALIZED AS
FROM _get_continuous_estimates!(cpu3_mw) |> EXTEND 3 AS cpu;

CREATE PERFETTO PIPELINE _system_state_cpu4_mw MATERIALIZED AS
FROM _get_continuous_estimates!(cpu4_mw) |> EXTEND 4 AS cpu;

CREATE PERFETTO PIPELINE _system_state_cpu5_mw MATERIALIZED AS
FROM _get_continuous_estimates!(cpu5_mw) |> EXTEND 5 AS cpu;

CREATE PERFETTO PIPELINE _system_state_cpu6_mw MATERIALIZED AS
FROM _get_continuous_estimates!(cpu6_mw) |> EXTEND 6 AS cpu;

CREATE PERFETTO PIPELINE _system_state_cpu7_mw MATERIALIZED AS
FROM _get_continuous_estimates!(cpu7_mw) |> EXTEND 7 AS cpu;

CREATE PERFETTO PIPELINE _system_state_dsu_scu_mw MATERIALIZED AS
FROM _get_continuous_estimates!(dsu_scu_mw);

CREATE PERFETTO PIPELINE _system_state_gpu_mw MATERIALIZED AS
FROM _get_continuous_estimates!(gpu_mw);

CREATE PERFETTO PIPELINE _system_state_tpu_mw MATERIALIZED AS
FROM _get_continuous_estimates!(tpu_mw);
