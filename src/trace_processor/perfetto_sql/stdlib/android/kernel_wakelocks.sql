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
--

-- NOTE (psqlnext): `counters.intervals` is DELETED — `counter_leading_intervals!`
-- is `INTERVALS FROM EVENTS` (+`MERGE CONSECUTIVE BY value`); the partitioned
-- `span_join` with `android_suspend_state` is `INTERVAL SPLIT ... PER name_int`.

INCLUDE PERFETTO MODULE android.suspend;

CREATE PERFETTO PIPELINE _kernel_wakelock_track MATERIALIZED AS
FROM track AS t
|> WHERE type = 'android_kernel_wakelock'
|> SELECT id, name, extract_arg(dimension_arg_set_id, 'wakelock_type') AS type;

CREATE PERFETTO PIPELINE _android_kernel_wakelocks_base MATERIALIZED AS
SUBPIPELINE kernel_wakelock_events AS (
  FROM counter
  |> WHERE track_id IN (SELECT id FROM _kernel_wakelock_track)
  |> SELECT id, ts, track_id, value
)
-- Counter samples become leading intervals; `next_value` is the following
-- sample's value (the held total at the next change) reached via lane order.
INTERVALS FROM EVENTS kernel_wakelock_events PER track_id CLOSING LAST AT (trace_end())
|> INTERVAL MERGE CONSECUTIVE BY value
|> EXTEND LEAD(value) OVER (PARTITION BY track_id ORDER BY ts) AS next_value
|> JOIN _kernel_wakelock_track AS t
   ON t.id = track_id
|> SELECT
     ts,
     ts AS original_ts,
     dur,
     t.name AS name,
     hash(t.name) AS name_int,
     t.type AS type,
     next_value - value AS held_dur;

-- Table of kernel (or native) wakelocks with held duration.
--
-- Subtracts suspended time from each period to calculate the
-- fraction of awake time for which the wakelock was held.
CREATE PERFETTO PIPELINE android_kernel_wakelocks(
  -- Timestamp.
  ts TIMESTAMP,
  -- Duration.
  dur DURATION,
  -- Duration spent awake.
  awake_dur DURATION,
  -- Kernel or native wakelock name.
  name STRING,
  -- 'kernel' or 'native'.
  type STRING,
  -- Time the wakelock was held.
  held_dur DURATION,
  -- Fraction of awake (not suspended) time the wakelock was held.
  held_ratio DOUBLE
)
MATERIALIZED AS
-- Split each wakelock period by suspend state, so each fragment knows whether
-- the device was awake; re-aggregate back to one row per original period.
FROM _android_kernel_wakelocks_base
|> INTERVAL SPLIT android_suspend_state AS s PER name_int
|> AGGREGATE
     sum(dur) AS dur,
     sum(iif(s.power_state = 'awake', dur, 0)) AS awake_dur
   GROUP BY original_ts AS ts, name, type, held_dur
|> SELECT
     ts,
     dur,
     awake_dur,
     name,
     type,
     cast_int!(held_dur) AS held_dur,
     max(min(held_dur / awake_dur, 1.0), 0.0) AS held_ratio;
