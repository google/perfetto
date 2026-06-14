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

-- Gets devfreq frequency counter based on device queried. These counters will
-- only be available if the "devfreq/devfreq_frequency" ftrace event is enabled.
-- The counter leading-intervals (events -> intervals, closing the last open
-- interval at the trace end, then coalescing consecutive equal values) are
-- expressed inline as a pipeline-valued macro.
CREATE PERFETTO MACRO _get_devfreq_counters(
  -- Devfreq name to query for.
  device_name Expr
)
-- Returns a pipeline of (id LONG, ts TIMESTAMP, dur DURATION, freq LONG).
RETURNS Pipeline
AS (
  INTERVALS FROM CHANGES (
    FROM counter AS c
    |> JOIN track AS t ON t.id = c.track_id
    |> WHERE t.type = 'linux_device_frequency'
       AND EXTRACT_ARG(t.dimension_arg_set_id, 'linux_device') GLOB $device_name
  ) CLOSING LAST AT (trace_end())
  |> INTERVAL MERGE CONSECUTIVE BY value AGGREGATE MIN(id) AS id
  |> SELECT id, ts, dur, cast_int!(value) AS freq
);

-- ARM DSU device frequency counters. This table will only be populated on
-- traces collected with "devfreq/devfreq_frequency" ftrace event enabled,
-- and from ARM devices with the DSU (DynamIQ Shared Unit) hardware.
CREATE PERFETTO PIPELINE linux_devfreq_dsu_counter(
  -- Unique identifier for this counter.
  id LONG,
  -- Starting timestamp of the counter.
  ts TIMESTAMP,
  -- Duration in which counter is constant and frequency doesn't chamge.
  dur DURATION,
  -- Frequency in kHz of the device that corresponds to the counter.
  dsu_freq LONG
) MATERIALIZED AS
_get_devfreq_counters!('*devfreq_dsu')
|> SELECT id, ts, dur, freq AS dsu_freq
|> UNION ALL (
  _get_devfreq_counters!('*dsufreq')
  |> SELECT id, ts, dur, freq AS dsu_freq
);
