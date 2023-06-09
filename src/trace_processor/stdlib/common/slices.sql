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
