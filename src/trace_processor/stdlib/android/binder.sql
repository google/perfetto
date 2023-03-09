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
  binder_txn AS (
    SELECT
      slice.id AS binder_txn_id,
      process.name AS process_name,
      thread.name AS thread_name,
      thread.utid AS utid,
      process.upid AS upid,
      slice.ts,
      slice.dur,
      thread.is_main_thread
    FROM slice
    INNER JOIN thread_track
      ON slice.track_id = thread_track.id
    INNER JOIN thread
      USING (utid)
    INNER JOIN process
      USING (upid)
    LEFT JOIN slice non_existent ON non_existent.parent_id = slice.id
    WHERE slice.name = 'binder transaction'
    AND non_existent.depth IS NULL
  ),
  binder_reply AS (
    SELECT
      binder_txn.*,
      binder_reply.ts AS server_ts,
      binder_reply.dur AS server_dur,
      binder_reply.id AS binder_reply_id,
      IIF(aidl.name LIKE 'AIDL::%' AND aidl.depth = binder_reply.depth + 1, aidl.name, NULL) AS aidl_name,
      reply_thread.name AS server_thread,
      reply_process.name AS server_process,
      reply_thread.utid AS server_utid,
      reply_process.upid AS server_upid
    FROM binder_txn
    INNER JOIN flow binder_flow
      ON binder_txn.binder_txn_id = binder_flow.slice_out
    INNER JOIN slice binder_reply
      ON binder_flow.slice_in = binder_reply.id
    INNER JOIN thread_track reply_thread_track ON binder_reply.track_id = reply_thread_track.id
    INNER JOIN thread reply_thread ON reply_thread.utid = reply_thread_track.utid
    INNER JOIN process reply_process ON reply_process.upid = reply_thread.upid
    LEFT JOIN slice aidl ON aidl.parent_id = binder_reply.id
  )
SELECT
  MIN(aidl_name) AS aidl_name,
  binder_txn_id,
  process_name AS client_process,
  thread_name AS client_thread,
  upid AS client_upid,
  utid AS client_utid,
  is_main_thread,
  ts AS client_ts,
  dur AS client_dur,
  binder_reply_id,
  server_process,
  server_thread,
  server_upid,
  server_utid,
  server_ts,
  server_dur
FROM binder_reply
WHERE client_dur >= 0 AND server_dur >= 0 AND client_dur >= server_dur
GROUP BY
  process_name,
  thread_name,
  binder_txn_id,
  binder_reply_id;

-- Breakdown synchronous binder transactions per txn.
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
-- @column thread_state_type whether thread state is on the txn or reply side
-- @column thread_state_ts ts of the txn thread_state
-- @column thread_state_dur dur of the txn thread_state
-- @column thread_state actual thread state of the txn thread_state
-- @column blocked_function blocked kernel function in the txn thread_state
CREATE VIEW android_sync_binder_txn_thread_state_by_txn
AS
SELECT
  binder.*,
  'binder_txn' AS thread_state_type,
  txn_state.state AS thread_state,
  SPANS_OVERLAPPING_DUR(txn_state.ts, txn_state.dur, client_ts, client_dur) AS thread_state_dur,
  blocked_function
FROM android_sync_binder_metrics_by_txn binder
INNER JOIN thread_state txn_state
  ON (
    txn_state.utid = binder.client_utid)
    AND (
      (txn_state.ts + txn_state.dur BETWEEN client_ts AND client_ts + client_dur)
      OR (client_ts + client_dur BETWEEN txn_state.ts AND txn_state.ts + txn_state.dur));

-- Breakdown synchronous binder transactions per reply.
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
-- @column thread_state_type whether thread state is on the txn or reply side
-- @column thread_state_ts ts of the reply thread_state
-- @column thread_state_dur dur of the reply thread_state
-- @column thread_state actual thread state of the reply thread_state
-- @column blocked_function blocked kernel function in the reply thread_state
CREATE VIEW android_sync_binder_reply_thread_state_by_txn
AS
SELECT
  binder.*,
  'binder_reply' AS thread_state_type,
  reply_state.state AS thread_state,
  SPANS_OVERLAPPING_DUR(reply_state.ts, reply_state.dur, server_ts, server_dur) AS thread_state_dur,
  blocked_function
FROM android_sync_binder_metrics_by_txn binder
INNER JOIN thread_state reply_state
  ON (
    reply_state.utid = binder.server_utid)
    AND (
      (reply_state.ts + reply_state.dur BETWEEN server_ts AND server_ts + server_dur)
      OR (server_ts + server_dur BETWEEN reply_state.ts AND reply_state.ts + reply_state.dur));

-- Aggregated thread_states on the client and server side per binder txn
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
  binder_reply_id,
  thread_state_type,
  thread_state,
  SUM(thread_state_dur) AS thread_state_dur,
  COUNT(thread_state_dur) AS thread_state_count
FROM android_sync_binder_txn_thread_state_by_txn
GROUP BY binder_txn_id, binder_reply_id, thread_state_type, thread_state
UNION ALL
SELECT
  binder_txn_id,
  binder_reply_id,
  thread_state_type,
  thread_state,
  SUM(thread_state_dur) AS thread_state_dur,
  COUNT(thread_state_dur) AS thread_state_count
FROM android_sync_binder_reply_thread_state_by_txn
GROUP BY binder_txn_id, binder_reply_id, thread_state_type, thread_state;

-- Aggregated blocked_functions on the client and server side per binder txn
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
  binder_reply_id,
  thread_state_type,
  blocked_function,
  SUM(thread_state_dur) AS blocked_function_dur,
  COUNT(thread_state_dur) AS blocked_function_count
FROM android_sync_binder_txn_thread_state_by_txn
WHERE blocked_function IS NOT NULL
GROUP BY binder_txn_id, binder_reply_id, blocked_function
UNION ALL
SELECT
  binder_txn_id,
  binder_reply_id,
  thread_state_type,
  blocked_function,
  SUM(thread_state_dur) AS blocked_function_dur,
  COUNT(thread_state_dur) AS blocked_function_count
FROM android_sync_binder_reply_thread_state_by_txn
WHERE blocked_function IS NOT NULL
GROUP BY binder_txn_id, binder_reply_id, blocked_function;
