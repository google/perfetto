--
-- Copyright 2023 The Android Open Source Project
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

-- Compute the distribution of the overlap of the given intervals over time.
--
-- Each interval is a (ts, dur) pair and the overlap represented as a (ts, value)
-- counter, with the value corresponding to the number of intervals that overlap
-- the given timestamp and interval until the next timestamp.
CREATE PERFETTO MACRO intervals_overlap_count(
    -- Table or subquery containing interval data.
    segments TableOrSubquery,
    -- Column containing interval starts (usually `ts`).
    ts_column ColumnName,
    -- Column containing interval durations (usually `dur`).
    dur_column ColumnName)
-- The returned table has the schema (ts INT64, value UINT32).
-- |ts| is the timestamp when the number of open segments changed. |value| is
-- the number of open segments.
RETURNS TableOrSubquery AS
(
-- Algorithm: for each segment, emit a +1 at the start and a -1 at the end.
-- Then, merge events with the same timestamp and compute a cumulative sum.
WITH
_starts AS (
  SELECT
    1 AS delta,
    $ts_column AS ts
  FROM $segments
),
_ends AS (
  SELECT
    -1 AS delta,
    $ts_column + $dur_column AS ts
  FROM $segments
  WHERE $dur_column != -1
),
_events AS (
  SELECT * FROM _starts
  UNION ALL
  SELECT * FROM _ends
),
-- Merge events with the same timestamp to avoid artifacts in the data.
_merged_events AS (
  SELECT ts, sum(delta) as delta
  FROM _events
  GROUP BY ts
)
SELECT
  ts,
  sum(delta) OVER (
    ORDER BY ts
    ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
  ) as value
FROM _merged_events
ORDER BY ts
);