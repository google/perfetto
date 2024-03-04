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

-- TODO(mayzner): Replace with good implementation of interval intersect.
CREATE PERFETTO MACRO _interval_intersect_partition_utid(
  left_table TableOrSubquery,
  right_table TableOrSubquery
)
RETURNS TableOrSubquery AS
(
  WITH on_left AS (
    SELECT
      B.ts,
      IIF(
        A.ts + A.dur <= B.ts + B.dur,
        A.ts + A.dur - B.ts, B.dur) AS dur,
      A.id AS left_id,
      B.id as right_id
    FROM $left_table A
    JOIN $right_table B ON (A.ts <= B.ts AND A.ts + A.dur > B.ts AND A.utid = B.utid)
  ), on_right AS (
    SELECT
      B.ts,
      IIF(
        A.ts + A.dur <= B.ts + B.dur,
        A.ts + A.dur - B.ts, B.dur) AS dur,
      B.id as left_id,
      A.id AS right_id
    FROM $right_table A
    -- The difference between this table and on_left is the lack of equality on
    -- A.ts <= B.ts. This is to remove the issue of double accounting
    -- timestamps that start at the same time.
    JOIN $left_table B ON (A.ts < B.ts AND A.ts + A.dur > B.ts AND A.utid = B.utid)
  )
  SELECT * FROM on_left
  UNION ALL
  SELECT * FROM on_right
);

-- Time each thread slice spent running on CPU.
-- Requires scheduling data to be available in the trace.
CREATE PERFETTO TABLE thread_slice_cpu_time(
    -- Slice id.
    id INT,
    -- Duration of the time the slice was running.
    cpu_time INT) AS
WITH slice_with_utid AS (
  SELECT
      slice.id,
      slice.ts,
      slice.dur,
      utid
  FROM slice
  JOIN thread_track ON slice.track_id = thread_track.id
  JOIN thread USING (utid)
  WHERE utid != 0)
SELECT left_id AS id, SUM(dur) AS cpu_time
FROM _interval_intersect_partition_utid!(slice_with_utid, sched)
GROUP BY 1
ORDER BY 1;