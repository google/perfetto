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
CREATE PERFETTO VIEW thread_slice(
  -- Alias for `slice.id`.
  id INT,
  -- Alias for `slice.type`.
  type STRING,
  -- Alias for `slice.ts`.
  ts INT,
  -- Alias for `slice.dur`.
  dur INT,
  -- Alias for `slice.category`.
  category STRING,
  -- Alias for `slice.name`.
  name STRING,
  -- Alias for `slice.track_id`.
  track_id INT,
  -- Alias for `thread_track.name`.
  track_name STRING,
  -- Alias for `thread.name`.
  thread_name STRING,
  -- Alias for `thread.utid`.
  utid INT,
  -- Alias for `thread.tid`
  tid INT,
  -- Alias for `process.name`.
  process_name STRING,
  -- Alias for `process.upid`.
  upid INT,
  -- Alias for `process.pid`.
  pid INT,
  -- Alias for `slice.depth`.
  depth INT,
  -- Alias for `slice.parent_id`.
  parent_id INT,
  -- Alias for `slice.arg_set_id`.
  arg_set_id INT,
  -- Alias for `slice.thread_ts`.
  thread_ts INT,
  -- Alias for `slice.thread_dur`.
  thread_dur INT
) AS
SELECT
  slice.id,
  slice.type,
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
CREATE PERFETTO VIEW process_slice(
  -- Alias for `slice.id`.
  id INT,
  -- Alias for `slice.type`.
  type STRING,
  -- Alias for `slice.ts`.
  ts INT,
  -- Alias for `slice.dur`.
  dur INT,
  -- Alias for `slice.category`.
  category STRING,
  -- Alias for `slice.name`.
  name STRING,
  -- Alias for `slice.track_id`.
  track_id INT,
  -- Alias for `process_track.name`.
  track_name STRING,
  -- Alias for `process.name`.
  process_name STRING,
  -- Alias for `process.upid`.
  upid INT,
  -- Alias for `process.pid`.
  pid INT,
  -- Alias for `slice.depth`.
  depth INT,
  -- Alias for `slice.parent_id`.
  parent_id INT,
  -- Alias for `slice.arg_set_id`.
  arg_set_id INT,
  -- Alias for `slice.thread_ts`.
  thread_ts INT,
  -- Alias for `slice.thread_dur`.
  thread_dur INT
) AS
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
CREATE PERFETTO FUNCTION has_parent_slice_with_name(
  -- Id of the slice to check parents of.
  id INT,
  -- Name of potential ancestor slice.
  parent_name STRING)
-- Whether `parent_name` is a name of an ancestor slice.
RETURNS BOOL AS
SELECT EXISTS(
  SELECT 1
  FROM ancestor_slice($id)
  WHERE name = $parent_name
  LIMIT 1
);

-- Checks if slice has a descendant with provided name.
CREATE PERFETTO FUNCTION has_descendant_slice_with_name(
  -- Id of the slice to check descendants of.
  id INT,
  -- Name of potential descendant slice.
  descendant_name STRING
)
-- Whether `descendant_name` is a name of an descendant slice.
RETURNS BOOL AS
SELECT EXISTS(
  SELECT 1
  FROM descendant_slice($id)
  WHERE name = $descendant_name
  LIMIT 1
);

-- Count slices with specified name.
CREATE PERFETTO FUNCTION slice_count(
  -- Name of the slices to counted.
  slice_glob STRING)
-- Number of slices with the name.
RETURNS INT AS
SELECT COUNT(1) FROM slice WHERE name GLOB $slice_glob;;

-- Finds the end timestamp for a given slice's descendant with a given name.
-- If there are multiple descendants with a given name, the function will return the
-- first one, so it's most useful when working with a timeline broken down into phases,
-- where each subphase can happen only once.
CREATE PERFETTO FUNCTION descendant_slice_end(
  -- Id of the parent slice.
  parent_id INT,
  -- Name of the child with the desired end TS.
  child_name STRING
)
-- End timestamp of the child or NULL if it doesn't exist.
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
CREATE PERFETTO FUNCTION direct_children_slice(
  -- Id of the parent slice.
  parent_id LONG)
RETURNS TABLE(
  -- Alias for `slice.id`.
  id LONG,
  -- Alias for `slice.type`.
  type STRING,
  -- Alias for `slice.ts`.
  ts LONG,
  -- Alias for `slice.dur`.
  dur LONG,
  -- Alias for `slice.category`.
  category LONG,
  -- Alias for `slice.name`.
  name STRING,
  -- Alias for `slice.track_id`.
  track_id LONG,
  -- Alias for `slice.depth`.
  depth LONG,
  -- Alias for `slice.parent_id`.
  parent_id LONG,
  -- Alias for `slice.arg_set_id`.
  arg_set_id LONG,
  -- Alias for `slice.thread_ts`.
  thread_ts LONG,
  -- Alias for `slice.thread_dur`.
  thread_dur LONG
) AS
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
CREATE PERFETTO FUNCTION slice_name_from_id(
  id LONG
)
-- The name of slice with the given id.
RETURNS STRING AS
SELECT
  name
FROM slice
WHERE $id = id;