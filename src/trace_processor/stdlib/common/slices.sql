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

--
-- Thread slices view functions
--

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
CREATE VIEW all_thread_slices AS
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

-- Detailed thread slices data with process, thread and track for thread with provided utid.
--
-- @arg utid INT              Utid of thread.
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
SELECT CREATE_VIEW_FUNCTION(
'THREAD_SLICES_FOR_UTID(utid INT)',
  '
    slice_id INT,
    slice_name STRING,
    ts LONG,
    dur LONG,
    slice_depth INT,
    arg_set_id INT,
    thread_track_id INT,
    thread_track_name STRING,
    utid INT,
    thread_name STRING,
    upid INT,
    process_name STRING
  ',
'
  SELECT * FROM all_thread_slices
  WHERE utid = $utid;
');

-- Detailed thread slices data with process, thread and track for process with provided upid.
--
-- @arg upid INT              Upid of process.
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
SELECT CREATE_VIEW_FUNCTION(
  'THREAD_SLICES_FOR_UPID(upid INT)',
  '
    slice_id INT,
    slice_name STRING,
    ts LONG,
    dur LONG,
    slice_depth INT,
    arg_set_id INT,
    thread_track_id INT,
    thread_track_name STRING,
    utid INT,
    thread_name STRING,
    upid INT,
    process_name STRING
  ',
  '
    SELECT * FROM all_thread_slices
    WHERE upid = $upid;
  '
);

-- Detailed thread slices data with process, thread and track for track id.
--
-- @arg thread_track_id INT   Id of thread_track.
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
SELECT CREATE_VIEW_FUNCTION(
  'THREAD_SLICES_FOR_THREAD_TRACK_ID(thread_track_id INT)',
  '
    slice_id INT,
    slice_name STRING,
    ts LONG,
    dur LONG,
    slice_depth INT,
    arg_set_id INT,
    thread_track_id INT,
    thread_track_name STRING,
    utid INT,
    thread_name STRING,
    upid INT,
    process_name STRING
  ',
  '
    SELECT * FROM all_thread_slices
    WHERE thread_track_id = $thread_track_id;
  '
);


-- Detailed thread slices data with process, thread and track for specified slice name.
-- Searches for slice name with GLOB.
--
-- @arg glob_name STRING      String name to glob.
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
SELECT CREATE_VIEW_FUNCTION(
  'THREAD_SLICES_FOR_SLICE_NAME(glob_name STRING)',
  '
    slice_id INT,
    slice_name STRING,
    ts LONG,
    dur LONG,
    slice_depth INT,
    arg_set_id INT,
    thread_track_id INT,
    thread_track_name STRING,
    utid INT,
    thread_name STRING,
    upid INT,
    process_name STRING
  ',
  '
    SELECT * FROM all_thread_slices
    WHERE slice_name GLOB $glob_name;
  ');

-- Detailed thread slices data with process, thread and track for specified thread name.
-- Searches for thread name with GLOB.
--
-- @arg glob_name STRING      Thread name to glob.
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
SELECT CREATE_VIEW_FUNCTION(
  'THREAD_SLICES_FOR_THREAD_NAME(glob_name STRING)',
  '
    slice_id INT,
    slice_name STRING,
    ts LONG,
    dur LONG,
    slice_depth INT,
    arg_set_id INT,
    thread_track_id INT,
    thread_track_name STRING,
    utid INT,
    thread_name STRING,
    upid INT,
    process_name STRING
  ',
  '
    SELECT * FROM all_thread_slices
    WHERE thread_name GLOB $glob_name;
  ');

-- Detailed thread slices data with process, thread and track for specified process name.
-- Searches for process name with GLOB.
--
-- @arg glob_name STRING      Process name to glob.
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
SELECT CREATE_VIEW_FUNCTION(
  'THREAD_SLICES_FOR_PROCESS_NAME(glob_name STRING)',
  '
    slice_id INT,
    slice_name STRING,
    ts LONG,
    dur LONG,
    slice_depth INT,
    arg_set_id INT,
    thread_track_id INT,
    thread_track_name STRING,
    utid INT,
    thread_name STRING,
    upid INT,
    process_name STRING
  ',
  '
    SELECT * FROM all_thread_slices
    WHERE process_name GLOB $glob_name;
  ');

--
-- Process slices view functions
--

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
CREATE VIEW all_process_slices AS
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

-- Detailed process slices data for process and track with provided upid.
--
-- @arg upid INT              Upid of process.
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
SELECT CREATE_VIEW_FUNCTION(
  'PROCESS_SLICES_FOR_UPID(upid INT)',
  '
    slice_id INT,
    slice_name STRING,
    ts LONG,
    dur LONG,
    slice_depth INT,
    arg_set_id INT,
    process_track_id INT,
    process_track_name INT,
    upid INT,
    process_name STRING
  ',
  '
    SELECT * FROM all_process_slices
    WHERE upid = $upid;
  '
);

-- Detailed process slices data for process and track with provided process_track_id.
--
-- @arg process_track_id INT  Id of process track.
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
SELECT CREATE_VIEW_FUNCTION(
  'PROCESS_SLICES_FOR_PROCESS_TRACK_ID(process_track_id INT)',
  '
    slice_id INT,
    slice_name STRING,
    ts LONG,
    dur LONG,
    slice_depth INT,
    arg_set_id INT,
    process_track_id INT,
    process_track_name INT,
    upid INT,
    process_name STRING
  ',
  '
    SELECT * FROM all_process_slices
    WHERE process_track_id = $process_track_id;
  '
);


-- Detailed process slices data for specified slice name.
-- Searches for slice name with GLOB.
--
-- @arg glob_name STRING      String name to glob.
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
SELECT CREATE_VIEW_FUNCTION(
  'PROCESS_SLICES_FOR_SLICE_NAME(glob_name STRING)',
  '
    slice_id INT,
    slice_name STRING,
    ts LONG,
    dur LONG,
    slice_depth INT,
    arg_set_id INT,
    process_track_id INT,
    process_track_name INT,
    upid INT,
    process_name STRING
  ',
  '
    SELECT * FROM all_process_slices
    WHERE slice_name GLOB $glob_name;
  '
);

-- Detailed process slices data for specified process name.
-- Searches for process name with GLOB.
--
-- @arg glob_name STRING      Process name to glob.
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
SELECT CREATE_VIEW_FUNCTION(
  'PROCESS_SLICES_FOR_PROCESS_NAME(glob_name STRING)',
  '
    slice_id INT,
    slice_name STRING,
    ts LONG,
    dur LONG,
    slice_depth INT,
    arg_set_id INT,
    process_track_id INT,
    process_track_name INT,
    upid INT,
    process_name STRING
  ',
  '
    SELECT * FROM all_process_slices
    WHERE process_name GLOB $glob_name;
  '
);


--
-- Other functions
--

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
