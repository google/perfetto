--
-- Copyright 2026 The Android Open Source Project
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

INCLUDE PERFETTO MODULE wattson.gpu.estimates;

INCLUDE PERFETTO MODULE wattson.gpu.freq_idle;

INCLUDE PERFETTO MODULE wattson.tasks.gpu_tasks;

INCLUDE PERFETTO MODULE wattson.utils;

-- Step 1: Find active GPU regions (contiguous freq > 0)
CREATE PERFETTO PIPELINE _gpu_active_regions MATERIALIZED AS
FROM _gpu_freq_idle
|> SELECT
  ts,
  dur,
  freq > 0 AS is_active,
  lag(freq > 0) OVER (ORDER BY ts) AS prev_active
-- Sum transitions to create group IDs
|> EXTEND
  sum(CASE WHEN is_active != coalesce(prev_active, 0) THEN 1 ELSE 0 END) OVER (
    ORDER BY ts
  ) AS group_id
|> WHERE is_active = 1
|> AGGREGATE
  min(ts) AS ts, max(ts + dur) - min(ts) AS dur
  GROUP BY group_id;

-- Step 2: Find tasks within active regions
CREATE PERFETTO PIPELINE _gpu_active_region_tasks MATERIALIZED AS
INTERVAL INTERSECTION OF (_gpu_active_regions AS r, _gpu_tasks AS ta)
|> SELECT ts, dur, ta.uid, r.group_id AS region_id;

-- Step 3: Find active region task boundaries
CREATE PERFETTO PIPELINE _gpu_active_region_boundaries AS
FROM _gpu_active_region_tasks
|> EXTEND row_number() OVER (PARTITION BY region_id ORDER BY ts) AS rnk_asc
|> EXTEND row_number() OVER (PARTITION BY region_id ORDER BY ts DESC) AS rnk_desc
|> AGGREGATE
  min(ts) AS min_ts,
  max(ts + dur) AS max_end_ts,
  min(CASE WHEN rnk_asc = 1 THEN uid END) AS first_uid,
  min(CASE WHEN rnk_desc = 1 THEN uid END) AS last_uid
  GROUP BY region_id;

-- Step 4: Classify gaps within active regions. A gap is the part of an active
-- region not covered by any GPU task, i.e. the region with the task coverage
-- removed.
CREATE PERFETTO PIPELINE _gaps_in_active_regions MATERIALIZED AS
FROM _gpu_active_regions AS r
|> INTERVAL SUBTRACT _gpu_tasks
|> SELECT ts, dur, r.group_id AS region_id;

CREATE PERFETTO PIPELINE _gpu_active_region_gaps MATERIALIZED AS
FROM _gaps_in_active_regions AS g
|> JOIN _gpu_active_region_boundaries AS b ON g.region_id = b.region_id
|> SELECT
  g.ts,
  g.dur,
  CASE
    WHEN b.min_ts IS NULL THEN -1
    WHEN g.ts + g.dur <= b.min_ts THEN b.first_uid
    ELSE -1
  END AS uid;

-- Step 5: Final Gap Attribution with Power
CREATE PERFETTO PIPELINE _gpu_gap_attribution MATERIALIZED AS
INTERVAL INTERSECTION OF (_gpu_active_region_gaps AS ta, _gpu_estimates_mw AS p)
|> SELECT ts, dur, coalesce(ta.uid, -1) AS uid, p.gpu_mw AS estimated_mw;
