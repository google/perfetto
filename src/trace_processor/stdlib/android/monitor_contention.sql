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

-- Extracts the blocking thread tid from a slice name
--
-- @arg slice_name STRING   Name of slice
-- @ret INT                 Blocking thread tid
SELECT
  CREATE_FUNCTION(
    'ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_BLOCKING_TID(slice_name STRING)',
    'INT',
    '
    SELECT CAST(STR_SPLIT(STR_SPLIT($slice_name, " (", 1), ")", 0) AS INT)
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

-- Contains parsed monitor contention slices.
--
-- @column blocking_method Name of the method holding the lock.
-- @column blocked_methhod Name of the method trying to acquire the lock.
-- @column short_blocking_method Blocking_method without arguments and return types.
-- @column short_blocked_method Blocked_method without arguments and return types.
-- @column blocking_src File location of blocking_method in form <filename:linenumber>.
-- @column blocked_src File location of blocked_method in form <filename:linenumber>.
-- @column waiter_count Zero indexed number of threads trying to acquire the lock.
-- @column blocking_utid Utid of thread holding the lock.
-- @column blocking_thread_name Thread name of thread holding the lock.
-- @column upid Upid of process experiencing lock contention.
-- @column process_name Process name of process experiencing lock contention.
-- @column id Slice id of lock contention.
-- @column ts Timestamp of lock contention start.
-- @column dur Duration of lock contention.
-- @column track_id Thread track id of blocked thread.
-- @column is_blocked_main_thread Whether the blocked thread is the main thread.
-- @column is_blocking_main_thread Whether the blocking thread is the main thread.
-- @column binder_reply_id Slice id of binder reply slice if lock contention was part of a binder txn.
-- @column binder_reply_ts Timestamp of binder reply slice if lock contention was part of a binder txn.
-- @column binder_reply_tid Tid of binder reply slice if lock contention was part of a binder txn.
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
  ANDROID_EXTRACT_ANDROID_MONITOR_CONTENTION_BLOCKING_TID(slice.name) AS blocking_tid,
  thread.upid AS upid,
  process.name AS process_name,
  slice.id,
  slice.ts,
  slice.dur,
  slice.track_id,
  thread.is_main_thread AS is_blocked_thread_main,
  blocking_thread.is_main_thread AS is_blocking_thread_main,
  binder_reply.id AS binder_reply_id,
  binder_reply.ts AS binder_reply_ts,
  binder_reply_thread.tid AS binder_reply_tid
FROM slice
JOIN thread_track
  ON thread_track.id = slice.track_id
LEFT JOIN thread
  USING (utid)
LEFT JOIN process
  USING (upid)
LEFT JOIN internal_broken_android_monitor_contention ON internal_broken_android_monitor_contention.id = slice.id
LEFT JOIN ANCESTOR_SLICE(slice.id) binder_reply ON binder_reply.name = 'binder reply'
LEFT JOIN thread_track binder_reply_thread_track ON binder_reply.track_id = binder_reply_thread_track.id
LEFT JOIN thread binder_reply_thread ON binder_reply_thread_track.utid = binder_reply_thread.utid
JOIN thread blocking_thread ON blocking_thread.tid = blocking_tid AND blocking_thread.upid = thread.upid
WHERE slice.name LIKE 'monitor contention%'
  AND slice.dur != -1
  AND internal_broken_android_monitor_contention.id IS NULL
  AND short_blocking_method IS NOT NULL
  AND short_blocked_method IS NOT NULL
GROUP BY slice.id;

-- Contains parsed monitor contention slices with the parent-child relationships.
--
-- @column parent_id Id of slice blocking the blocking_thread.
-- @column blocking_method Name of the method holding the lock.
-- @column blocked_methhod Name of the method trying to acquire the lock.
-- @column short_blocking_method Blocking_method without arguments and return types.
-- @column short_blocked_method Blocked_method without arguments and return types.
-- @column blocking_src File location of blocking_method in form <filename:linenumber>.
-- @column blocked_src File location of blocked_method in form <filename:linenumber>.
-- @column waiter_count Zero indexed number of threads trying to acquire the lock.
-- @column blocking_utid Utid of thread holding the lock.
-- @column blocking_thread_name Thread name of thread holding the lock.
-- @column upid Upid of process experiencing lock contention.
-- @column process_name Process name of process experiencing lock contention.
-- @column id Slice id of lock contention.
-- @column ts Timestamp of lock contention start.
-- @column dur Duration of lock contention.
-- @column track_id Thread track id of blocked thread.
-- @column is_blocked_main_thread Whether the blocked thread is the main thread.
-- @column is_blocking_main_thread Whether the blocking thread is the main thread.
-- @column binder_reply_id Slice id of binder reply slice if lock contention was part of a binder txn.
-- @column binder_reply_ts Timestamp of binder reply slice if lock contention was part of a binder txn.
-- @column binder_reply_tid Tid of binder reply slice if lock contention was part of a binder txn.
CREATE TABLE android_monitor_contention_chain
AS
SELECT parent.id AS parent_id, child.* FROM android_monitor_contention child
LEFT JOIN android_monitor_contention parent ON child.blocked_utid = parent.blocking_utid
    AND parent.ts BETWEEN child.ts AND child.ts + child.dur;

CREATE VIEW internal_blocking_thread_state
AS
SELECT utid AS blocking_utid, ts, dur, state, blocked_function
FROM thread_state;

-- Contains the span join of the |android_monitor_contention_chain| with their
-- blocking thread thread state.
--
-- @column parent_id Id of slice blocking the blocking_thread.
-- @column blocking_method Name of the method holding the lock.
-- @column blocked_methhod Name of the method trying to acquire the lock.
-- @column short_blocking_method Blocking_method without arguments and return types.
-- @column short_blocked_method Blocked_method without arguments and return types.
-- @column blocking_src File location of blocking_method in form <filename:linenumber>.
-- @column blocked_src File location of blocked_method in form <filename:linenumber>.
-- @column waiter_count Zero indexed number of threads trying to acquire the lock.
-- @column blocking_utid Utid of thread holding the lock.
-- @column blocking_thread_name Thread name of thread holding the lock.
-- @column upid Upid of process experiencing lock contention.
-- @column process_name Process name of process experiencing lock contention.
-- @column id Slice id of lock contention.
-- @column ts Timestamp of lock contention start.
-- @column dur Duration of lock contention.
-- @column track_id Thread track id of blocked thread.
-- @column is_blocked_main_thread Whether the blocked thread is the main thread.
-- @column is_blocking_main_thread Whether the blocking thread is the main thread.
-- @column binder_reply_id Slice id of binder reply slice if lock contention was part of a binder txn.
-- @column binder_reply_ts Timestamp of binder reply slice if lock contention was part of a binder txn.
-- @column binder_reply_tid Tid of binder reply slice if lock contention was part of a binder txn.
-- @column blocking_utid Utid of the blocking |thread_state|.
-- @column ts Timestamp of the blocking |thread_state|.
-- @column state Thread state of the blocking thread.
-- @column blocked_function Blocked kernel function of the blocking thread.
CREATE VIRTUAL TABLE android_monitor_contention_chain_thread_state
USING
  SPAN_JOIN(android_monitor_contention_chain PARTITIONED blocking_utid,
            internal_blocking_thread_state PARTITIONED blocking_utid);

-- Aggregated blocked_functions on the 'blocking thread', the thread holding the lock.
-- This builds on the data from |android_monitor_contention_chain| and
-- for each contention slice, it returns the aggregated sum of all the thread states on the
-- blocking thread.
--
-- @column id Slice id of the monitor contention.
-- @column thread_state A |thread_state| that occurred in the blocking thread during the contention.
-- @column thread_state_dur Total time the blocking thread spent in the |thread_state| during
-- contention.
-- @column thread_state_count Count of all times the blocking thread entered |thread_state| during
-- the contention.
CREATE VIEW android_monitor_contention_chain_thread_state_by_txn
AS
SELECT
  id,
  state AS thread_state,
  SUM(dur) AS thread_state_dur,
  COUNT(dur) AS thread_state_count
FROM android_monitor_contention_chain_thread_state
GROUP BY id, thread_state;

-- Aggregated blocked_functions on the 'blocking thread', the thread holding the lock.
-- This builds on the data from |android_monitor_contention_chain| and
-- for each contention, it returns the aggregated sum of all the kernel
-- blocked function durations on the blocking thread.
--
-- @column id Slice id of the monitor contention.
-- @column blocked_function Blocked kernel function in a thread state in the blocking thread during
-- the contention.
-- @column blocked_function_dur Total time the blocking thread spent in the |blocked_function|
-- during the contention.
-- @column blocked_function_count Count of all times the blocking thread executed the
-- |blocked_function| during the contention.
CREATE VIEW android_monitor_contention_chain_blocked_functions_by_txn
AS
SELECT
  id,
  blocked_function,
  SUM(dur) AS blocked_function_dur,
  COUNT(dur) AS blocked_function_count
FROM android_monitor_contention_chain_thread_state
WHERE blocked_function IS NOT NULL
GROUP BY id, blocked_function;
