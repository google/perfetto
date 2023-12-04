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

-- The concept of a "flat slice" is to take the data in the slice table and
-- remove all notion of nesting; we do this by projecting every slice in a stack to
-- their ancestor slice, i.e at any point in time, taking the  most specific active
-- slice (i.e. the slice at the bottom of the stack) and representing that as the
-- *only* slice that was running during that period.
--
-- This concept becomes very useful when you try and linearise a trace and
-- compare it with other traces spanning the same user action; "self time" (i.e.
-- time spent in a slice but *not* any children) is easily computed and span
-- joins with thread state become possible without limiting to only depth zero
--- slices.
--
-- Note that, no slices will be generated for intervals without without any slices.
--
-- As an example, consider the following slice stack:
-- A-------------B.
-- ----C----D----.
-- The flattened slice will be: A----C----D----B.
--
-- @column slice_id           Id of most active slice.
-- @column ts                 Timestamp when `slice.id` became the most active slice.
-- @column dur                Duration of `slice.id` as the most active slice until the next active slice.
-- @column depth              Depth of `slice.id` in the original stack.
-- @column name               Name of `slice.id`.
-- @column root_name          Name of the top most slice of the stack.
-- @column root_id            Id of of the top most slice of the stack.
-- @column track_id           Alias for `slice.track_id`.
-- @column utid               Alias for `thread.utid`.
-- @column tid                Alias for `thread.tid`
-- @column thread_name        Alias for `thread.name`.
-- @column upid               Alias for `process.upid`.
-- @column pid                Alias for `process.pid`.
-- @column process_name       Alias for `process.name`.
CREATE TABLE experimental_slice_flattened AS
-- The algorithm proceeds as follows:
-- 1. Find the start and end timestamps of all slices.
-- 2. Iterate the generated timestamps within a stack in chronoligical order.
-- 3. Generate a slice for each timestamp pair (regardless of if it was a start or end)  .
-- 4. If the first timestamp in the pair was originally a start, the slice is the 'current' slice,
-- otherwise, the slice is the parent slice.
WITH
  begins AS (
    SELECT id AS slice_id, ts, name, track_id, depth
    FROM slice
    WHERE dur > 0
  ),
  ends AS (
    SELECT
      parent.id AS slice_id,
      current.ts + current.dur AS ts,
      parent.name as name,
      current.track_id,
      current.depth - 1 AS depth
    FROM slice current
    LEFT JOIN slice parent
      ON current.parent_id = parent.id
    WHERE current.dur > 0
  ),
  events AS (
    SELECT * FROM begins
    UNION ALL
    SELECT * FROM ends
  ),
  data AS (
    SELECT
      events.slice_id,
      events.ts,
      LEAD(events.ts) OVER (
         PARTITION BY events.track_id
         ORDER BY events.ts) - events.ts AS dur,
      events.depth,
      events.name,
      events.track_id
    FROM events
  )
SELECT data.slice_id, data.ts, data.dur, data.depth,
 data.name, data.track_id, thread.utid, thread.tid, thread.name as thread_name,
 process.upid, process.pid, process.name as process_name
 FROM data JOIN thread_track ON data.track_id = thread_track.id
JOIN thread USING(utid)
JOIN process USING(upid)
WHERE depth != -1;

CREATE
  INDEX experimental_slice_flattened_id_idx
ON experimental_slice_flattened(slice_id);

CREATE
  INDEX experimental_slice_flattened_ts_idx
ON experimental_slice_flattened(ts);
