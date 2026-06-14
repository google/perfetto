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
--

-- Provides a timeseries of "Bitmap Memory" counter for each process, which
-- is useful for retrieving the total memory used by bitmaps by an application over time.
--
-- To populate this table, tracing must be enabled with the "view" atrace
-- category.
CREATE PERFETTO PIPELINE android_bitmap_memory(
  -- ID of the row in the underlying counter table.
  id ID(counter.id),
  -- Upid of the process.
  upid JOINID(process.upid),
  -- Timestamp of the start of the interval.
  ts TIMESTAMP,
  -- Duration of the interval.
  dur DURATION,
  -- Duration of the interval.
  track_id JOINID(counter.track_id),
  -- Memory consumed by bitmaps in bytes.
  value LONG
)
MATERIALIZED AS
SUBPIPELINE evts AS (
  FROM counter AS c
  |> JOIN process_counter_track AS pct ON pct.id = c.track_id
  |> WHERE pct.name = 'Bitmap Memory'
  |> SELECT c.id, c.track_id, c.ts, c.value
)
INTERVALS FROM EVENTS evts PER track_id CLOSING LAST AT (trace_end())
|> INTERVAL MERGE CONSECUTIVE BY value AGGREGATE MIN(id) AS id
|> JOIN process_counter_track AS pct ON pct.id = track_id
|> SELECT id, upid, ts, dur, track_id, value
|> ORDER BY id;

-- Provides a timeseries of "Bitmap Count" counter for each process, which
-- is useful for retrieving the number of bitmaps used by an application over time.
--
-- To populate this table, tracing must be enabled with the "view" atrace
-- category.
CREATE PERFETTO PIPELINE android_bitmap_count(
  -- ID of the row in the underlying counter table.
  id ID(counter.id),
  -- Upid of the process.
  upid JOINID(process.upid),
  -- Timestamp of the start of the interval.
  ts TIMESTAMP,
  -- Duration of the interval.
  dur DURATION,
  -- Duration of the interval.
  track_id JOINID(counter.track_id),
  -- Number of allocated bitmaps.
  value LONG
)
MATERIALIZED AS
SUBPIPELINE evts AS (
  FROM counter AS c
  |> JOIN process_counter_track AS pct ON pct.id = c.track_id
  |> WHERE pct.name = 'Bitmap Count'
  |> SELECT c.id, c.track_id, c.ts, c.value
)
INTERVALS FROM EVENTS evts PER track_id CLOSING LAST AT (trace_end())
|> INTERVAL MERGE CONSECUTIVE BY value AGGREGATE MIN(id) AS id
|> JOIN process_counter_track AS pct ON pct.id = track_id
|> SELECT id, upid, ts, dur, track_id, value
|> ORDER BY id;

-- Provides a timeseries of bitmap-related counters for each process, which
-- is useful for understanding an application's bitmap usage over time.
--
-- To populate this table, tracing must be enabled with the "view" atrace
-- category.
CREATE PERFETTO PIPELINE android_bitmap_counters_per_process(
  -- Upid of the process.
  upid JOINID(process.upid),
  -- Name of the process.
  process_name STRING,
  -- Timestamp of the start of the interval.
  ts TIMESTAMP,
  -- Duration of the interval.
  dur DURATION,
  -- Memory consumed by bitmaps in bytes.
  bitmap_memory LONG,
  -- Number of allocated bitmaps.
  bitmap_count LONG,
  -- ID of the row in the underlying counter table.
  bitmap_memory_id JOINID(counter.id),
  -- ID of the row in the underlying counter table.
  bitmap_count_id JOINID(counter.id)
)
MATERIALIZED AS
-- TODO(lalitm): we have this interval intersect because as implemented today,
-- the bitmap memory and count counters are updated one after the
-- other *but* with slightly different timestamps. Ideally, we would remove
-- these "intermediate" intervals but that would require heuristics. So for now,
-- we just intersect the intervals together and retain the intermediate
-- intervals. Alternatively, if we had a way to actually timestamp both
-- counters at the same time, we could avoid this. We would need the Perfetto
-- SDK for that though.
INTERVAL INTERSECTION OF (
  android_bitmap_memory AS abm,
  android_bitmap_count AS abc
) PER upid
|> JOIN process AS p USING (upid)
|> SELECT
  p.upid,
  p.name AS process_name,
  ts,
  dur,
  abm.value AS bitmap_memory,
  abc.value AS bitmap_count,
  abm.id AS bitmap_memory_id,
  abc.id AS bitmap_count_id;
