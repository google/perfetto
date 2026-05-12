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

INCLUDE PERFETTO MODULE counters.intervals;

INCLUDE PERFETTO MODULE android.suspend;

CREATE PERFETTO TABLE _kernel_wakelock_track AS
SELECT id, name, extract_arg(dimension_arg_set_id, 'wakelock_type') AS type
FROM track AS t
WHERE
  type = 'android_kernel_wakelock';

-- Create a VIEW to defer the cost of counter_leading_intervals.
-- This VIEW calculates the duration and value changes for kernel wakelock counters.
CREATE PERFETTO VIEW _kernel_wakelock_intervals AS
SELECT ts, dur, track_id, value, next_value
FROM counter_leading_intervals!((
    SELECT id, ts, track_id, value
    FROM counter
    WHERE
      track_id IN (SELECT id FROM _kernel_wakelock_track)
  ));

-- This base table joins the interval data from the VIEW with the track information.
-- The expensive computation from counter_leading_intervals is only triggered when this table
-- (or tables depending on it) is queried.
CREATE PERFETTO TABLE _android_kernel_wakelocks_base AS
SELECT
  i.ts,
  i.ts AS original_ts,
  i.dur,
  t.name,
  hash(t.name) AS name_int,
  t.type,
  i.next_value - i.value AS held_dur
FROM _kernel_wakelock_intervals AS i
JOIN _kernel_wakelock_track AS t
  ON t.id = i.track_id;

CREATE VIRTUAL TABLE _android_kernel_wakelocks_joined USING span_join(_android_kernel_wakelocks_base partitioned name_int, android_suspend_state);

-- Table of kernel (or native) wakelocks with held duration.
--
-- Subtracts suspended time from each period to calculate the
-- fraction of awake time for which the wakelock was held.
CREATE PERFETTO TABLE android_kernel_wakelocks(
  -- Timestamp of the start of the interval.
  ts TIMESTAMP,
  -- Duration of the interval.
  dur DURATION,
  -- Duration within the interval spent awake (not suspended).
  awake_dur DURATION,
  -- Kernel or native wakelock name.
  name STRING,
  -- Type of wakelock, e.g., 'kernel' or 'native'.
  type STRING,
  -- Time the wakelock was actively held during the interval.
  held_dur DURATION,
  -- Fraction of the awake duration (awake_dur) that the wakelock was held.
  held_ratio DOUBLE
)
AS
WITH
  base AS (
    SELECT
      original_ts AS ts,
      name,
      type,
      held_dur,
      sum(dur) AS dur,
      sum(iif(power_state = 'awake', dur, 0)) AS awake_dur
    FROM _android_kernel_wakelocks_joined
    GROUP BY
      original_ts,
      name,
      type,
      held_dur
  )
SELECT
  ts,
  dur,
  awake_dur,
  name,
  type,
  cast_int!(held_dur) AS held_dur,
  max(min(held_dur / awake_dur, 1.0), 0.0) AS held_ratio
FROM base;
