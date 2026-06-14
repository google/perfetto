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

-- NOTE (psqlnext): the `_binder_client_view`/`_binder_server_view`/
-- `_thread_state_view` projections, the `_binder_flatten_descendants` macro, the
-- `_binder_*_flat_descendants[_with_thread_state]` tables, and the
-- `_binder_*_breakdown_view` projections are all GONE — folded into the three
-- pipelines below as inline SUBPIPELINEs. Only `_binder_reason` (a scalar
-- function) remains.

INCLUDE PERFETTO MODULE android.binder;

INCLUDE PERFETTO MODULE slices.with_context;

-- Returns the 'reason' for a binder txn delay based on the descendant slice
-- name and thread_state information. It follows the following priority:
-- 1. direct_reclaim.
-- 2. GC blocking stall.
-- 3. Sleeping in monitor contention.
-- 4. Sleeping in ART lock contention.
-- 5. Sleeping in binder txn or reply.
-- 6. Sleeping in Mutex contention.
-- 7. IO.
-- 8. State itself.
CREATE PERFETTO FUNCTION _binder_reason(
  name STRING,
  state STRING,
  io_wait LONG,
  binder_txn_id LONG,
  binder_reply_id LONG,
  flat_id LONG
)
RETURNS STRING
AS
SELECT
  CASE
    WHEN $name = 'mm_vmscan_direct_reclaim' THEN 'kernel_memory_reclaim'
    WHEN $name GLOB 'GC: Wait For*' THEN 'userspace_memory_reclaim'
    WHEN $state = 'S'
    AND ($name GLOB 'monitor contention*'
    OR $name GLOB 'Lock contention on a monitor lock*') THEN 'monitor_contention'
    WHEN $state = 'S'
    AND ($name GLOB 'Lock contention*') THEN 'art_lock_contention'
    WHEN $state = 'S'
    AND $binder_reply_id != $flat_id
    AND $binder_txn_id != $flat_id
    AND ($name = 'binder transaction' OR $name = 'binder reply') THEN 'binder'
    WHEN $state = 'S'
    AND ($name = 'Contending for pthread mutex') THEN 'mutex_contention'
    WHEN $state != 'S'
    AND $io_wait = 1 THEN 'io'
    ELSE $state
  END AS name;

-- Server side binder breakdowns per transactions per txn.
CREATE PERFETTO PIPELINE android_binder_server_breakdown(
  -- Client side id of the binder txn.
  binder_txn_id JOINID(slice.id),
  -- Server side id of the binder txn.
  binder_reply_id JOINID(slice.id),
  -- Timestamp of an exclusive interval during the binder reply with a single
  -- reason.
  ts TIMESTAMP,
  -- Duration of an exclusive interval during the binder reply with a single
  -- reason.
  dur DURATION,
  -- Cause of delay during an exclusive interval of the binder reply.
  reason STRING
)
MATERIALIZED AS
-- The 'binder reply' slice and everything nested under it, projected onto the
-- timeline so each disjoint segment is labelled by its deepest (innermost) live
-- slice, tagged with the reply/txn ids it belongs to. (Was the
-- `_binder_flatten_descendants!` macro + `_binder_server_flat_descendants`.)
SUBPIPELINE reply_roots AS (
  FROM slice |> WHERE name = 'binder reply' |> SELECT id
)
SUBPIPELINE segments AS (
  FROM thread_slice
  -- Subtree (root included) of each 'binder reply'.
  |> TREE KEEP IF DESCENDANT OF reply_roots OVER slice
  -- Carry the reply slice id (= binder_reply_id) down to every descendant.
  |> TREE ACCUMULATE DOWN
       FIRST(iif(name = 'binder reply', id, NULL)) AS binder_reply_id
  |> JOIN android_binder_txns AS txn USING (binder_reply_id)
  -- Deepest live slice per disjoint segment, per reply.
  |> INTERVAL FLATTEN PER binder_reply_id, binder_txn_id, utid
       AGGREGATE ARG_MAX(depth, name) AS name, ARG_MAX(depth, id) AS flat_id
  |> WHERE dur > 0
)
-- Intersect each segment with the server thread's scheduling state.
INTERVAL INTERSECTION OF (segments AS seg, thread_state AS st) PER utid
|> SELECT
     seg.binder_txn_id AS binder_txn_id,
     seg.binder_reply_id AS binder_reply_id,
     ts,
     dur,
     _binder_reason(
       seg.name, st.state, st.io_wait,
       seg.binder_txn_id, seg.binder_reply_id, seg.flat_id
     ) AS reason;

-- Client side binder breakdowns per transactions per txn.
CREATE PERFETTO PIPELINE android_binder_client_breakdown(
  -- Client side id of the binder txn.
  binder_txn_id JOINID(slice.id),
  -- Server side id of the binder txn.
  binder_reply_id JOINID(slice.id),
  -- Timestamp of an exclusive interval during the binder txn with a single
  -- latency reason.
  ts TIMESTAMP,
  -- Duration of an exclusive interval during the binder txn with a single
  -- latency reason.
  dur DURATION,
  -- Cause of delay during an exclusive interval of the binder txn.
  reason STRING
)
MATERIALIZED AS
-- Same shape as the server side, rooted at the 'binder transaction' slice.
SUBPIPELINE txn_roots AS (
  FROM slice |> WHERE name = 'binder transaction' |> SELECT id
)
SUBPIPELINE segments AS (
  FROM thread_slice
  |> TREE KEEP IF DESCENDANT OF txn_roots OVER slice
  -- The transaction slice id is the binder_txn_id.
  |> TREE ACCUMULATE DOWN
       FIRST(iif(name = 'binder transaction', id, NULL)) AS binder_txn_id
  |> JOIN android_binder_txns AS txn USING (binder_txn_id)
  |> INTERVAL FLATTEN PER binder_txn_id, binder_reply_id, utid
       AGGREGATE ARG_MAX(depth, name) AS name, ARG_MAX(depth, id) AS flat_id
  |> WHERE dur > 0
)
INTERVAL INTERSECTION OF (segments AS seg, thread_state AS st) PER utid
|> SELECT
     seg.binder_txn_id AS binder_txn_id,
     seg.binder_reply_id AS binder_reply_id,
     ts,
     dur,
     _binder_reason(
       seg.name, st.state, st.io_wait,
       seg.binder_txn_id, seg.binder_reply_id, seg.flat_id
     ) AS reason;

-- Combined client and server side binder breakdowns per transaction.
CREATE PERFETTO PIPELINE android_binder_client_server_breakdown(
  -- Client side id of the binder txn.
  binder_txn_id JOINID(slice.id),
  -- Server side id of the binder txn.
  binder_reply_id JOINID(slice.id),
  -- Timestamp of an exclusive interval during the binder txn with a single
  -- latency reason.
  ts TIMESTAMP,
  -- Duration of an exclusive interval during the binder txn with a single
  -- latency reason.
  dur DURATION,
  -- The server side component of this interval's binder latency reason, if any.
  server_reason STRING,
  -- The client side component of this interval's binder latency reason.
  client_reason STRING,
  -- Combined reason indicating whether latency came from client or server side.
  reason STRING,
  -- Whether the latency is due to the client or server.
  reason_type STRING
)
MATERIALIZED AS
-- Was SPAN_LEFT_JOIN PARTITIONED binder_txn_id: the client breakdown is the left
-- spine, split at the server breakdown's boundaries so the server reason attaches
-- where the two overlap and is null elsewhere.
FROM android_binder_client_breakdown
|> INTERVAL SPLIT android_binder_server_breakdown AS server PER binder_txn_id
|> SELECT
     binder_txn_id,
     binder_reply_id,
     ts,
     dur,
     server.reason AS server_reason,
     reason AS client_reason,
     iif(server.reason IS NOT NULL, server.reason, reason) AS reason,
     iif(server.reason IS NOT NULL, 'server', 'client') AS reason_type;
