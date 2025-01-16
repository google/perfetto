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

include perfetto module android.suspend;

-- Table of kernel (or native) wakelocks with held duration.
--
-- Subtracts suspended time from each period to calculate the
-- fraction of awake time for which the wakelock was held.
CREATE PERFETTO TABLE android_kernel_wakelocks(
  -- Timestamp.
  ts TIMESTAMP,
  -- Duration.
  dur DURATION,
  -- Kernel or native wakelock name.
  name STRING,
  -- 'kernel' or 'native'.
  type STRING,
  -- Time the wakelock was held.
  held_dur DURATION,
  -- Fraction of awake (not suspended) time the wakelock was held.
  held_ratio DOUBLE) AS
with raw as (
  select
    -- Move back by one period here since our ts represents wakelock counts up
    -- to that point, but counters project forward from their ts.
    lag(ts) over (partition by track_id order by ts) as ts,
    ts - lag(ts) over (partition by track_id order by ts) as dur,
    name,
    extract_arg(dimension_arg_set_id, 'wakelock_type') as type,
    value - lag(value) over (partition by track_id order by ts) as held_dur
  from track t join counter c on t.id = c.track_id
  where t.type = 'android_kernel_wakelock'
),
suspended as (
  select
    ts,
    dur,
    name,
    type,
    ifnull((select sum(dur) from android_suspend_state s
        where power_state = 'suspended'
          and s.ts > raw.ts - dur
          and s.ts < raw.ts), 0) as suspended_dur,
    held_dur
  from raw
  where dur is not null
)
select
  ts,
  dur,
  name,
  type,
  cast_int!(held_dur) as held_dur,
  min(held_dur / (dur - suspended_dur), 1.0) as held_ratio
from suspended;
