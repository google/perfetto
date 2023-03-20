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
--

-- Extracts the blocking thread from a slice name
--
-- @arg slice_name STRING   Name of slice
-- @ret STRING              Blocking thread
SELECT
  CREATE_FUNCTION(
    'ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_BLOCKING_THREAD(slice_name STRING)',
    'STRING',
    '
    SELECT STR_SPLIT(STR_SPLIT($slice_name, "with owner ", 1), " (", 0)
  '
);

-- Extracts the blocking method from a slice name
--
-- @arg slice_name STRING   Name of slice
-- @ret STRING              Blocking thread
SELECT
  CREATE_FUNCTION(
    'ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_BLOCKING_METHOD(slice_name STRING)',
    'STRING',
    '
    SELECT STR_SPLIT(STR_SPLIT($slice_name, ") at ", 1), "(", 0)
    || "("
    || STR_SPLIT(STR_SPLIT($slice_name, ") at ", 1), "(", 1)
  '
);

-- Extracts a shortened form of the blocking method name from a slice name.
-- The shortened form discards the parameter and return
-- types.
--
-- @arg slice_name STRING   Name of slice
-- @ret STRING              Blocking thread
SELECT
  CREATE_FUNCTION(
    'ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_SHORT_BLOCKING_METHOD(slice_name STRING)',
    'STRING',
    '
    SELECT
    STR_SPLIT(STR_SPLIT(ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_BLOCKING_METHOD($slice_name), " ", 1), "(", 0)
  '
);

-- Extracts the monitor contention blocked method from a slice name
--
-- @arg slice_name STRING   Name of slice
-- @ret STRING              Blocking thread
SELECT
  CREATE_FUNCTION(
    'ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_BLOCKED_METHOD(slice_name STRING)',
    'STRING',
    '
    SELECT STR_SPLIT(STR_SPLIT($slice_name, "blocking from ", 1), "(", 0)
    || "("
    || STR_SPLIT(STR_SPLIT($slice_name, "blocking from ", 1), "(", 1)
  '
);

-- Extracts a shortened form of the monitor contention blocked method name
-- from a slice name. The shortened form discards the parameter and return
-- types.
--
-- @arg slice_name STRING   Name of slice
-- @ret STRING              Blocking thread
SELECT
  CREATE_FUNCTION(
    'ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_SHORT_BLOCKED_METHOD(slice_name STRING)',
    'STRING',
    '
    SELECT
    STR_SPLIT(STR_SPLIT(ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_BLOCKED_METHOD($slice_name), " ", 1), "(", 0)
  '
);

-- Extracts the number of waiters on the monitor from a slice name
--
-- @arg slice_name STRING   Name of slice
-- @ret INT                 Count of waiters on the lock
SELECT
  CREATE_FUNCTION(
    'ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_WAITER_COUNT(slice_name STRING)',
    'INT',
    '
    SELECT CAST(STR_SPLIT(STR_SPLIT($slice_name, "waiters=", 1), " ", 0) AS INT)
  '
);

-- Extracts the monitor contention blocking source location from a slice name
--
-- @arg slice_name STRING   Name of slice
-- @ret STRING              Blocking thread
SELECT
  CREATE_FUNCTION(
    'ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_BLOCKING_SRC(slice_name STRING)',
    'STRING',
    '
    SELECT STR_SPLIT(STR_SPLIT($slice_name, ")(", 1), ")", 0)
  '
);

-- Extracts the monitor contention blocked source location from a slice name
--
-- @arg slice_name STRING   Name of slice
-- @ret STRING              Blocking thread
SELECT
  CREATE_FUNCTION(
    'ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_BLOCKED_SRC(slice_name STRING)',
    'STRING',
    '
    SELECT STR_SPLIT(STR_SPLIT($slice_name, ")(", 2), ")", 0)
  '
);

CREATE TABLE internal_broken_android_monitor_contention
AS
SELECT ancestor.parent_id AS id FROM slice
    JOIN slice ancestor ON ancestor.id = slice.parent_id
    WHERE ancestor.name LIKE 'Lock contention on a monitor lock%'
    GROUP BY ancestor.id;

-- Contains parsed monitor contention slices
--
-- @column blocking_method name of the method holding the lock
-- @column blocked_methhod name of the method trying to acquire the lock
-- @column short_blocking_method blocking_method without arguments and return types
-- @column short_blocked_method blocked_method without arguments and return types
-- @column blocking_src file location of blocking_method in form <filename:linenumber>
-- @column blocked_src file location of blocked_method in form <filename:linenumber>
-- @column waiter_count zero indexed number of threads trying to acquire the lock
-- @column blocking_utid utid of thread holding the lock
-- @column blocking_thread_name thread name of thread holding the lock
-- @column upid upid of process experiencing lock contention
-- @column process_name process name of process experiencing lock contention
-- @column id slice id of lock contention
-- @column ts timestamp of lock contention start
-- @column dur duration of lock contention
-- @column track_id thread track id of blocked thread
-- @column binder_reply_id slice id of binder reply slice if lock contention was part of a binder txn
CREATE TABLE android_monitor_contention
AS
SELECT
  ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_BLOCKING_METHOD(slice.name) AS blocking_method,
  ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_BLOCKED_METHOD(slice.name)  AS blocked_method,
  ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_SHORT_BLOCKING_METHOD(slice.name) AS short_blocking_method,
  ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_SHORT_BLOCKED_METHOD(slice.name)  AS short_blocked_method,
  ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_BLOCKING_SRC(slice.name) AS blocking_src,
  ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_BLOCKED_SRC(slice.name) AS blocked_src,
  ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_WAITER_COUNT(slice.name) AS waiter_count,
  thread.utid AS blocked_utid,
  thread.name AS blocked_thread_name,
  blocking_thread.utid AS blocking_utid,
  ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_BLOCKING_THREAD(slice.name) AS blocking_thread_name,
  thread.upid AS upid,
  process.name AS process_name,
  slice.id,
  slice.ts,
  slice.dur,
  slice.track_id,
  binder_reply.id AS binder_reply_id
FROM slice
JOIN thread_track
  ON thread_track.id = slice.track_id
LEFT JOIN thread
  USING (utid)
LEFT JOIN process
  USING (upid)
LEFT JOIN internal_broken_android_monitor_contention ON internal_broken_android_monitor_contention.id = slice.id
LEFT JOIN ANCESTOR_SLICE(slice.id) binder_reply ON binder_reply.name = 'binder reply'
JOIN thread blocking_thread ON blocking_thread.name = blocking_thread_name AND blocking_thread.upid = thread.upid
WHERE slice.name LIKE 'monitor contention%'
  AND slice.dur != -1
  AND internal_broken_android_monitor_contention.id IS NULL
  AND short_blocking_method IS NOT NULL
  AND short_blocked_method IS NOT NULL
GROUP BY slice.id;

-- Contains parsed monitor contention slices with the parent-child relationships
--
-- @column parent_id id of slice blocking the blocking_thread
-- @column blocking_method name of the method holding the lock
-- @column blocked_methhod name of the method trying to acquire the lock
-- @column short_blocking_method blocking_method without arguments and return types
-- @column short_blocked_method blocked_method without arguments and return types
-- @column blocking_src file location of blocking_method in form <filename:linenumber>
-- @column blocked_src file location of blocked_method in form <filename:linenumber>
-- @column waiter_count zero indexed number of threads trying to acquire the lock
-- @column blocking_utid utid of thread holding the lock
-- @column blocking_thread_name thread name of thread holding the lock
-- @column upid upid of process experiencing lock contention
-- @column process_name process name of process experiencing lock contention
-- @column id slice id of lock contention
-- @column ts timestamp of lock contention start
-- @column dur duration of lock contention
-- @column track_id thread track id of blocked thread
-- @column binder_reply_id slice id of binder reply slice if lock contention was part of a binder txn
CREATE TABLE android_monitor_contention_chain
AS
SELECT parent.id AS parent_id, child.* FROM android_monitor_contention child
LEFT JOIN android_monitor_contention parent ON child.blocked_utid = parent.blocking_utid
    AND parent.ts BETWEEN child.ts AND child.ts + child.dur;
