--
-- Copyright 2024 The Android Open Source Project
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

INCLUDE PERFETTO MODULE slices.with_context;
INCLUDE PERFETTO MODULE android.binder;
INCLUDE PERFETTO MODULE graphs.search;

-- Details of all Service#onBind dispatched events.
CREATE PERFETTO TABLE _bind_dispatch
AS
WITH
  next_sibling AS MATERIALIZED (
    SELECT *
    FROM
      graph_next_sibling!(
          (
            SELECT id AS node_id, parent_id AS node_parent_id, ts AS sort_key
            FROM slice
            WHERE dur = 0
          )
      )
  ),
  service AS (
    SELECT
      next_slice.id,
      next_slice.ts,
      next_slice.dur,
      next_slice.name,
      slice.utid,
      slice.name AS bind_seq_name
    FROM next_sibling
    JOIN thread_slice slice
      ON slice.id = next_sibling.node_id
    JOIN slice next_slice
      ON next_slice.id = next_sibling.next_node_id
  )
  SELECT
  id,
  ts,
  dur,
  utid,
  CAST(STR_SPLIT(STR_SPLIT(bind_seq_name, 'bindSeq=', 1), ' ', 0) AS INT) AS bind_seq
FROM service
WHERE bind_seq_name GLOB 'requestServiceBinding*' AND name = 'binder transaction async';

-- Details of all Service#onBind received events.
CREATE PERFETTO TABLE _bind_receive
AS
SELECT
  id,
  ts,
  dur,
  track_id,
  REPLACE(STR_SPLIT(STR_SPLIT(name, 'token=', 1), ' ', 0), 'ServiceRecord{', '') AS token,
  STR_SPLIT(STR_SPLIT(name, 'act=', 1), ' ', 0) AS act,
  STR_SPLIT(STR_SPLIT(name, 'cmp=', 1), ' ', 0) AS cmp,
  STR_SPLIT(STR_SPLIT(name, 'flg=', 1), ' ', 0) AS flg,
  CAST(STR_SPLIT(STR_SPLIT(name, 'bindSeq=', 1), '}', 0) AS INT) AS bind_seq
FROM slice
WHERE name GLOB 'serviceBind:*';

-- All service bindings from client app to server app.
CREATE PERFETTO TABLE android_service_bindings(
  -- OOM score of client process making the binding.
  client_oom_score INT,
  -- Name of client process making the binding.
  client_process STRING,
  -- Name of client thread making the binding.
  client_thread STRING,
  -- Pid of client process making the binding.
  client_pid INT,
  -- Tid of client process making the binding.
  client_tid INT,
  -- Upid of client process making the binding.
  client_upid INT,
  -- Utid of client thread making the binding.
  client_utid INT,
  -- Timestamp the client process made the request.
  client_ts INT,
  -- Duration of the client binding request.
  client_dur INT,
  -- OOM score of server process getting bound to.
  server_oom_score INT,
  -- Name of server process getting bound to
  server_process STRING,
  -- Name of server thread getting bound to.
  server_thread STRING,
  -- Pid of server process getting bound to.
  server_pid INT,
  -- Tid of server process getting bound to.
  server_tid INT,
  -- Upid of server process getting bound to.
  server_upid INT,
  -- Utid of server process getting bound to.
  server_utid INT,
  -- Timestamp the server process got bound to.
  server_ts INT,
  -- Duration of the server process handling the binding.
  server_dur INT,
  -- Unique binder identifier for the Service binding.
  token STRING,
  -- Intent action name for the service binding.
  act STRING,
  -- Intent component name for the service binding.
  cmp STRING,
  -- Intent flag for the service binding.
  flg STRING,
  -- Monotonically increasing id for the service binding.
  bind_seq INT)
AS
SELECT
  COALESCE(client_binder.client_oom_score, server_binder.client_oom_score) AS client_oom_score,
  COALESCE(client_binder.client_process, server_binder.client_process) AS client_process,
  COALESCE(client_binder.client_thread, server_binder.client_thread) AS client_thread,
  COALESCE(client_binder.client_pid, server_binder.client_pid) AS client_pid,
  COALESCE(client_binder.client_tid, server_binder.client_tid) AS client_tid,
  COALESCE(client_binder.client_upid, server_binder.client_upid) AS client_upid,
  COALESCE(client_binder.client_utid, server_binder.client_utid) AS client_utid,
  COALESCE(client_binder.client_ts, server_binder.client_ts) AS client_ts,
  COALESCE(client_binder.client_dur, server_binder.client_dur) AS client_dur,
  server_binder.server_oom_score,
  server_binder.server_process,
  server_binder.server_thread,
  server_binder.server_pid,
  server_binder.server_tid,
  server_binder.server_upid,
  server_binder.server_utid,
  receive.ts AS server_ts,
  receive.dur AS server_dur,
  receive.token,
  receive.act,
  receive.cmp,
  receive.flg,
  receive.bind_seq
FROM _bind_dispatch dispatch
JOIN _bind_receive receive
  ON dispatch.bind_seq = receive.bind_seq
LEFT JOIN android_binder_txns server_binder
  ON server_binder.binder_txn_id = dispatch.id
LEFT JOIN ancestor_slice(dispatch.id) anc ON anc.depth = 0
LEFT JOIN android_binder_txns client_binder
  ON client_binder.server_ts = anc.ts AND dispatch.utid = client_binder.server_utid;
