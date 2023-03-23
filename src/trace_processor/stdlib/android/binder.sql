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

SELECT IMPORT('common.timestamps');

-- Count Binder transactions per process.
--
-- @column process_name  Name of the process that started the binder transaction.
-- @column pid           PID of the process that started the binder transaction.
-- @column slice_name    Name of the slice with binder transaction.
-- @column event_count   Number of binder transactions in process in slice.
CREATE VIEW android_binder_metrics_by_process AS
SELECT
  process.name AS process_name,
  process.pid AS pid,
  slice.name AS slice_name,
  COUNT(*) AS event_count
FROM slice
JOIN thread_track ON slice.track_id = thread_track.id
JOIN thread ON thread.utid = thread_track.utid
JOIN process ON thread.upid = process.upid
WHERE
  slice.name GLOB 'binder*'
GROUP BY
  process_name,
  slice_name;

-- Breakdown synchronous binder transactions per txn.
-- It returns data about the client and server ends of every binder transaction.
--
-- @column aidl_name name of the binder interface if existing
-- @column binder_txn_id slice id of the binder txn
-- @column client_process name of the client process
-- @column client_thread name of the client thread
-- @column client_upid name of the client upid
-- @column client_utid name of the client utid
-- @column client_ts timestamp of the client txn
-- @column client_dur dur of the client txn
-- @column is_main_thread Whether the txn was initiated from the main thread of the client process
-- @column binder_reply_id slice id of the binder reply
-- @column server_process name of the server process
-- @column server_thread  name of the server thread
-- @column server_upid name of the server upid
-- @column server_utid name of the server utid
-- @column server_ts timestamp of the server txn
-- @column server_dur dur of the server txn
CREATE VIEW android_sync_binder_metrics_by_txn AS
WITH
  -- Fetch the broken binder txns first, i.e, the txns that have children slices
  -- They are definietly broken because synchronous txns are blocked sleeping while
  -- waiting for a response.
  -- These broken txns will be excluded below in the binder_txn CTE
  broken_binder_txn AS (
    SELECT ancestor.id FROM slice
    JOIN slice ancestor ON ancestor.id = slice.parent_id
    WHERE ancestor.name = 'binder transaction'
    GROUP BY ancestor.id
  ),
  -- Adding MATERIALIZED here matters in cases where there are few/no binder
  -- transactions in the trace. Our cost estimation is not good enough to allow
  -- the query planner to see through to this fact. Instead, our cost estimation
  -- causes repeated queries on this table which is slow because it's an O(n)
  -- query.
  --
  -- We should fix this by doing some (ideally all) of the following:
  --  1) Add support for columnar tables in SQL which will allow for
  --     "subsetting" the slice table to only contain binder transactions.
  --  2) Make this query faster by adding improving string filtering.
  --  3) Add caching so that even if these queries happen many times, they are
  --     fast.
  --  4) Improve cost estimation algorithm to allow the joins to happen the
  --     right way around.
  binder_txn AS MATERIALIZED (
    SELECT
      slice.id AS binder_txn_id,
      process.name AS process_name,
      thread.name AS thread_name,
      thread.utid AS utid,
      thread.tid AS tid,
      process.upid AS upid,
      slice.ts,
      slice.dur,
      thread.is_main_thread
    FROM slice
    JOIN thread_track ON slice.track_id = thread_track.id
    JOIN thread USING (utid)
    JOIN process USING (upid)
    LEFT JOIN broken_binder_txn ON broken_binder_txn.id = slice.id
    WHERE slice.name = 'binder transaction'
    AND broken_binder_txn.id IS NULL
  ),
  binder_reply AS (
    SELECT
      binder_txn.*,
      binder_reply.ts AS server_ts,
      binder_reply.dur AS server_dur,
      binder_reply.id AS binder_reply_id,
      reply_thread.name AS server_thread,
      reply_process.name AS server_process,
      reply_thread.utid AS server_utid,
      reply_thread.tid AS server_tid,
      reply_process.upid AS server_upid,
      aidl.name AS aidl_name
    FROM binder_txn
    JOIN flow binder_flow ON binder_txn.binder_txn_id = binder_flow.slice_out
    JOIN slice binder_reply ON binder_flow.slice_in = binder_reply.id
    JOIN thread_track reply_thread_track
      ON binder_reply.track_id = reply_thread_track.id
    JOIN thread reply_thread ON reply_thread.utid = reply_thread_track.utid
    JOIN process reply_process ON reply_process.upid = reply_thread.upid
    LEFT JOIN slice aidl
      ON aidl.parent_id = binder_reply.id AND aidl.name LIKE 'AIDL::%'
  )
SELECT
  MIN(aidl_name) AS aidl_name,
  binder_txn_id,
  process_name AS client_process,
  thread_name AS client_thread,
  upid AS client_upid,
  utid AS client_utid,
  tid AS client_tid,
  is_main_thread,
  ts AS client_ts,
  dur AS client_dur,
  binder_reply_id,
  server_process,
  server_thread,
  server_upid,
  server_utid,
  server_tid,
  server_ts,
  server_dur
FROM binder_reply
WHERE client_dur != -1 AND server_dur != -1 AND client_dur >= server_dur
GROUP BY
  process_name,
  thread_name,
  binder_txn_id,
  binder_reply_id;

CREATE VIEW internal_binder_txn
AS
SELECT client_ts AS ts, client_dur AS dur, client_utid AS utid, *
FROM android_sync_binder_metrics_by_txn;

CREATE VIEW internal_binder_reply
AS
SELECT server_ts AS ts, server_dur AS dur, server_utid AS utid, *
FROM android_sync_binder_metrics_by_txn;

CREATE VIRTUAL TABLE internal_sp_binder_txn_thread_state
USING
  SPAN_JOIN(internal_binder_txn PARTITIONED utid, thread_state PARTITIONED utid);

CREATE VIRTUAL TABLE internal_sp_binder_reply_thread_state
USING
  SPAN_JOIN(internal_binder_reply PARTITIONED utid, thread_state PARTITIONED utid);

-- Aggregated thread_states on the client and server side per binder txn
-- This builds on the data from |android_sync_binder_metrics_by_txn| and
-- for each end (client and server) of the transaction, it returns
-- the aggregated sum of all the thread state durations.
-- The |thread_state_type| column represents whether a given 'aggregated thread_state'
-- row is on the client or server side. 'binder_txn' is client side and 'binder_reply'
-- is server side.
--
-- @column binder_txn_id slice id of the binder txn
-- @column binder_reply_id slice id of the binder reply
-- @column thread_state_type whether thread state is on the txn or reply side
-- @column thread_state a thread_state that occurred in the txn
-- @column thread_state_dur aggregated dur of the |thread_state| in the txn
-- @column thread_state_count aggregated count of the |thread_state| in the txn
CREATE VIEW android_sync_binder_thread_state_by_txn
AS
SELECT
  binder_txn_id,
  client_ts,
  client_tid,
  binder_reply_id,
  server_ts,
  server_tid,
  'binder_txn' AS thread_state_type,
  state AS thread_state,
  SUM(dur) AS thread_state_dur,
  COUNT(dur) AS thread_state_count
FROM internal_sp_binder_txn_thread_state
GROUP BY binder_txn_id, binder_reply_id, thread_state_type, thread_state
UNION ALL
SELECT
  binder_txn_id,
  client_ts,
  client_tid,
  binder_reply_id,
  server_ts,
  server_tid,
  'binder_reply' AS thread_state_type,
  state AS thread_state,
  SUM(dur) AS thread_state_dur,
  COUNT(dur) AS thread_state_count
FROM internal_sp_binder_reply_thread_state
GROUP BY binder_txn_id, binder_reply_id, thread_state_type, thread_state;

-- Aggregated blocked_functions on the client and server side per binder txn
-- This builds on the data from |android_sync_binder_metrics_by_txn| and
-- for each end (client and server) of the transaction, it returns
-- the aggregated sum of all the kernel blocked function durations.
-- The |thread_state_type| column represents whether a given 'aggregated blocked_function'
-- row is on the client or server side. 'binder_txn' is client side and 'binder_reply'
-- is server side.
--
-- @column binder_txn_id slice id of the binder txn
-- @column binder_reply_id slice id of the binder reply
-- @column thread_state_type whether thread state is on the txn or reply side
-- @column blocked_function blocked kernel function in a thread state
-- @column blocked_function_dur aggregated dur of the |blocked_function| in the txn
-- @column blocked_function_count aggregated count of the |blocked_function| in the txn
CREATE VIEW android_sync_binder_blocked_functions_by_txn
AS
SELECT
  binder_txn_id,
  client_ts,
  client_tid,
  binder_reply_id,
  server_ts,
  server_tid,
  'binder_txn' AS thread_state_type,
  blocked_function,
  SUM(dur) AS blocked_function_dur,
  COUNT(dur) AS blocked_function_count
FROM internal_sp_binder_txn_thread_state
WHERE blocked_function IS NOT NULL
GROUP BY binder_txn_id, binder_reply_id, blocked_function
UNION ALL
SELECT
  binder_txn_id,
  client_ts,
  client_tid,
  binder_reply_id,
  server_ts,
  server_tid,
  'binder_reply' AS thread_state_type,
  blocked_function,
  SUM(dur) AS blocked_function_dur,
  COUNT(dur) AS blocked_function_count
FROM internal_sp_binder_reply_thread_state
WHERE blocked_function IS NOT NULL
GROUP BY binder_txn_id, binder_reply_id, blocked_function;
