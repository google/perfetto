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
-- @column slice_id           Id of slice.
-- @column slice_name         Name of slice.
-- @column ts                 Timestamp of slice start.
-- @column dur                Duration of slice.
-- @column slice_depth        Depth of slice.
-- @column arg_set_id         Slice arg set id.
-- @column thread_track_id    Id of thread track.
-- @column thread_track_name  Name of thread track.
-- @column utid               Utid of thread with slice.
-- @column thread_name        Name of thread with slice.
-- @column upid               Upid of process with slice.
-- @column process_name       Name of process with slice.
CREATE VIEW thread_slice AS
SELECT
  slice.id AS slice_id,
  slice.name AS slice_name,
  ts,
  dur,
  slice.depth AS slice_depth,
  slice.arg_set_id,
  slice.track_id AS thread_track_id,
  thread_track.name AS thread_track_name,
  utid,
  thread.name AS thread_name,
  upid,
  process.name AS process_name
FROM slice
JOIN thread_track ON slice.track_id = thread_track.id
JOIN thread using (utid)
LEFT JOIN process using (upid);

-- All process slices with data about process track and process.
-- Where possible, use available view functions which filter this view.
--
-- @column slice_id           Id of slice.
-- @column slice_name         Name of slice.
-- @column ts                 Timestamp of slice start.
-- @column dur                Duration of slice.
-- @column slice_depth        Depth of slice.
-- @column arg_set_id         Slice arg set id.
-- @column process_track_id   Id of process track.
-- @column process_track_name Name of process track.
-- @column upid               Upid of process with slice.
-- @column process_name       Name of process with slice.
CREATE VIEW process_slice AS
SELECT
  slice.id AS slice_id,
  slice.name AS slice_name,
  ts,
  dur,
  slice.depth AS slice_depth,
  slice.arg_set_id,
  process_track.id AS process_track_id,
  process_track.name AS process_track_name,
  upid,
  process.name AS process_name
FROM slice
JOIN process_track ON slice.track_id = process_track.id
JOIN process using (upid);

-- Checks if slice has an ancestor with provided name.
--
-- @arg id INT              Id of the slice to check parents of.
-- @arg parent_name STRING  Name of potential ancestor slice.
-- @ret BOOL                Whether `parent_name` is a name of an ancestor slice.
SELECT
  CREATE_FUNCTION(
    'HAS_PARENT_SLICE_WITH_NAME(id INT, parent_name STRING)',
    'BOOL',
    '
    SELECT EXISTS(
      SELECT 1
      FROM ancestor_slice($id)
      WHERE name = $parent_name
      LIMIT 1
    );
  '
);

-- Count slices with specified name.
--
-- @arg slice_glob STRING Name of the slices to counted.
-- @ret INT               Number of slices with the name.
SELECT CREATE_FUNCTION(
  'SLICE_COUNT(slice_glob STRING)',
  'INT',
  'SELECT COUNT(1) FROM slice WHERE name GLOB $slice_glob;'
);
