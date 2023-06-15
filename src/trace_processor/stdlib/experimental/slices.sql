--
-- Copyright 2022 The Android Open Source Project
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

SELECT IMPORT('common.slices');

-- All slices with related process and thread info if available. Unlike
-- `thread_slice` and `process_slice`, this view contains all slices,
-- with thread- and process-related columns set to NULL if the slice
-- is not associated with a thread or a process.
--
-- @column id                 Alias for `slice.id`.
-- @column type               Alias for `slice.type`.
-- @column ts                 Alias for `slice.ts`.
-- @column dur                Alias for `slice.dur`.
-- @column category           Alias for `slice.category`.
-- @column name               Alias for `slice.name`.
-- @column track_id           Alias for `slice.track_id`.
-- @column track_name         Alias for `track.name`.
-- @column thread_name        Alias for `thread.name`.
-- @column utid               Alias for `thread.utid`.
-- @column tid                Alias for `thread.tid`
-- @column process_name       Alias for `process.name`.
-- @column upid               Alias for `process.upid`.
-- @column pid                Alias for `process.pid`.
-- @column depth              Alias for `slice.depth`.
-- @column parent_id          Alias for `slice.parent_id`.
-- @column arg_set_id         Alias for `slice.arg_set_id`.
-- @column thread_ts          Alias for `slice.thread_ts`.
-- @column thread_dur         Alias for `slice.thread_dur`.
CREATE VIEW experimental_slice_with_thread_and_process_info AS
SELECT
  slice.id,
  slice.type,
  slice.ts,
  slice.dur,
  slice.category,
  slice.name,
  slice.track_id,
  track.name AS track_name,
  thread.name AS thread_name,
  thread.utid,
  thread.tid,
  COALESCE(process1.name, process2.name) AS process_name,
  COALESCE(process1.upid, process2.upid) AS upid,
  COALESCE(process1.pid, process2.pid) AS pid,
  slice.depth,
  slice.parent_id,
  slice.arg_set_id,
  slice.thread_ts,
  slice.thread_dur
FROM slice
JOIN track ON slice.track_id = track.id
LEFT JOIN thread_track ON slice.track_id = thread_track.id
LEFT JOIN thread USING (utid)
LEFT JOIN process process1 ON thread.upid = process1.upid
LEFT JOIN process_track ON slice.track_id = process_track.id
LEFT JOIN process process2 ON process_track.upid = process2.upid;

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
CREATE TABLE experimental_slice_flattened
AS
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
    WHERE dur != -1
  ),
  ends AS (
    SELECT
      COALESCE(parent.id, current.id) AS slice_id,
      current.ts + current.dur AS ts,
      COALESCE(parent.name, current.name) AS name,
      current.track_id,
      COALESCE(parent.depth, 0) AS depth
    FROM slice current
    LEFT JOIN slice parent
      ON current.parent_id = parent.id
    WHERE current.dur != -1
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
      LEAD(events.ts)
        OVER (PARTITION BY COALESCE(anc.id, events.slice_id) ORDER BY events.ts) - events.ts AS dur,
      events.depth,
      events.name,
      COALESCE(anc.name, events.name) AS root_name,
      COALESCE(anc.id, events.slice_id) AS root_id,
      events.track_id,
      thread_slice.utid,
      thread_slice.tid,
      thread_slice.thread_name,
      thread_slice.upid,
      thread_slice.pid,
      thread_slice.process_name
    FROM events
    LEFT JOIN ANCESTOR_SLICE(events.slice_id) anc
      ON anc.depth = 0
    JOIN thread_slice ON thread_slice.id = events.slice_id
  )
SELECT * FROM data WHERE ts IS NOT NULL AND dur IS NOT NULL AND name IS NOT NULL;
