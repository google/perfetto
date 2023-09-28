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

-- All thread slices with data about thread, thread track and process.
-- Where possible, use available view functions which filter this view.
--
-- @column id                 Alias for `slice.id`.
-- @column type               Alias for `slice.type`.
-- @column ts                 Alias for `slice.ts`.
-- @column dur                Alias for `slice.dur`.
-- @column category           Alias for `slice.category`.
-- @column name               Alias for `slice.name`.
-- @column track_id           Alias for `slice.track_id`.
-- @column track_name         Alias for `thread_track.name`.
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
CREATE VIEW thread_slice AS
SELECT
  slice.id,
  slice.ts,
  slice.dur,
  slice.category,
  slice.name,
  slice.track_id,
  thread_track.name AS track_name,
  thread.name AS thread_name,
  thread.utid,
  thread.tid,
  process.name AS process_name,
  process.upid,
  process.pid,
  slice.depth,
  slice.parent_id,
  slice.arg_set_id,
  slice.thread_ts,
  slice.thread_dur
FROM slice
JOIN thread_track ON slice.track_id = thread_track.id
JOIN thread USING (utid)
LEFT JOIN process USING (upid);

-- All process slices with data about process track and process.
-- Where possible, use available view functions which filter this view.
--
-- @column id                 Alias for `slice.id`.
-- @column type               Alias for `slice.type`.
-- @column ts                 Alias for `slice.ts`.
-- @column dur                Alias for `slice.dur`.
-- @column category           Alias for `slice.category`.
-- @column name               Alias for `slice.name`.
-- @column track_id           Alias for `slice.track_id`.
-- @column track_name         Alias for `process_track.name`.
-- @column process_name       Alias for `process.name`.
-- @column upid               Alias for `process.upid`.
-- @column pid                Alias for `process.pid`.
-- @column depth              Alias for `slice.depth`.
-- @column parent_id          Alias for `slice.parent_id`.
-- @column arg_set_id         Alias for `slice.arg_set_id`.
-- @column thread_ts          Alias for `slice.thread_ts`.
-- @column thread_dur         Alias for `slice.thread_dur`.
CREATE VIEW process_slice AS
SELECT
  slice.id,
  slice.type,
  slice.ts,
  slice.dur,
  slice.category,
  slice.name,
  slice.track_id,
  process_track.name AS track_name,
  process.name AS process_name,
  process.upid,
  process.pid,
  slice.depth,
  slice.parent_id,
  slice.arg_set_id,
  slice.thread_ts,
  slice.thread_dur
FROM slice
JOIN process_track ON slice.track_id = process_track.id
JOIN process USING (upid);

-- Checks if slice has an ancestor with provided name.
--
-- @arg id INT              Id of the slice to check parents of.
-- @arg parent_name STRING  Name of potential ancestor slice.
-- @ret BOOL                Whether `parent_name` is a name of an ancestor slice.
CREATE PERFETTO FUNCTION has_parent_slice_with_name(id INT, parent_name STRING)
RETURNS BOOL AS
SELECT EXISTS(
  SELECT 1
  FROM ancestor_slice($id)
  WHERE name = $parent_name
  LIMIT 1
);

-- Checks if slice has a descendant with provided name.
--
-- @arg id INT                  Id of the slice to check descendants of.
-- @arg descendant_name STRING  Name of potential descendant slice.
-- @ret BOOL                    Whether `descendant_name` is a name of an
--                              descendant slice.
CREATE PERFETTO FUNCTION has_descendant_slice_with_name(
  id INT,
  descendant_name STRING
)
RETURNS BOOL AS
SELECT EXISTS(
  SELECT 1
  FROM descendant_slice($id)
  WHERE name = $descendant_name
  LIMIT 1
);

-- Count slices with specified name.
--
-- @arg slice_glob STRING Name of the slices to counted.
-- @ret INT               Number of slices with the name.
CREATE PERFETTO FUNCTION slice_count(slice_glob STRING)
RETURNS INT AS
SELECT COUNT(1) FROM slice WHERE name GLOB $slice_glob;;

-- Finds the end timestamp for a given slice's descendant with a given name.
-- If there are multiple descendants with a given name, the function will return the
-- first one, so it's most useful when working with a timeline broken down into phases,
-- where each subphase can happen only once.
-- @arg parent_id INT Id of the parent slice.
-- @arg child_name STRING name of the child with the desired end TS.
-- @ret INT end timestamp of the child or NULL if it doesn't exist.
CREATE PERFETTO FUNCTION descendant_slice_end(
  parent_id INT,
  child_name STRING
)
RETURNS INT AS
SELECT
  CASE WHEN s.dur
    IS NOT -1 THEN s.ts + s.dur
    ELSE NULL
  END
FROM descendant_slice($parent_id) s
WHERE s.name = $child_name
LIMIT 1;

-- Finds all slices with a direct parent with the given parent_id.
-- @arg parent_id INT Id of the parent slice.
-- @column id                 Alias for `slice.id`.
-- @column type               Alias for `slice.type`.
-- @column ts                 Alias for `slice.ts`.
-- @column dur                Alias for `slice.dur`.
-- @column category           Alias for `slice.category`.
-- @column name               Alias for `slice.name`.
-- @column track_id           Alias for `slice.track_id`.
-- @column depth              Alias for `slice.depth`.
-- @column parent_id          Alias for `slice.parent_id`.
-- @column arg_set_id         Alias for `slice.arg_set_id`.
-- @column thread_ts          Alias for `slice.thread_ts`.
-- @column thread_dur         Alias for `slice.thread_dur`.
CREATE PERFETTO FUNCTION direct_children_slice(parent_id LONG)
RETURNS TABLE(
  id LONG,
  type STRING,
  ts LONG,
  dur LONG,
  category LONG,
  name STRING,
  track_id LONG,
  depth LONG,
  parent_id LONG,
  arg_set_id LONG,
  thread_ts LONG,
  thread_dur LONG) AS
SELECT
  slice.id,
  slice.type,
  slice.ts,
  slice.dur,
  slice.category,
  slice.name,
  slice.track_id,
  slice.depth,
  slice.parent_id,
  slice.arg_set_id,
  slice.thread_ts,
  slice.thread_dur
FROM slice
WHERE parent_id = $parent_id;

-- Given a slice id, returns the name of the slice.
-- @arg id LONG the slice id which we need the name for.
-- @ret STRING the name of slice with the given id.
CREATE PERFETTO FUNCTION slice_name_from_id(
  id LONG
)
RETURNS STRING AS
SELECT
  name
FROM slice
WHERE $id = id;