--
-- Copyright 2026 The Android Open Source Project
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

-- Android work-passing critical path.
--
-- This module only defines the Android-specific rules for how work is passed
-- between threads (binder, vsync, monitor contention, ...). Building the
-- graph and walking it is done by the generic `sched.critical_path` module,
-- which reuses the C++ critical path walker.

INCLUDE PERFETTO MODULE sched.critical_path;

INCLUDE PERFETTO MODULE slices.with_context;

-- Work-passing edges: `ready_utid` made `utid` ready at `ready_ts` and `utid`
-- started the work at `ts`.
CREATE PERFETTO TABLE _android_pass_work(
  ready_ts TIMESTAMP,
  ready_utid JOINID(thread.id),
  ts TIMESTAMP,
  utid JOINID(thread.id),
  type STRING,
  info STRING
)
AS
-- thread_ready, if we don't match any work passing types, we follow the waker_utid on thread_ready
-- slices. With the exception that we don't follow if the previous state was uninterruptible sleep.
WITH
  thread_state_previous AS (
    SELECT *, lag(state) OVER (PARTITION BY utid ORDER BY ts ASC) AS prev_state
    FROM thread_state
  ),
  thread_ready AS (
    SELECT
      ts AS ready_ts,
      CASE WHEN irq_context = 1 THEN NULL ELSE waker_utid END AS ready_utid,
      ts + dur AS ts,
      utid,
      'thread_ready' AS type,
      '' AS info
    FROM thread_state_previous
    WHERE
      waker_utid IS NOT NULL
      -- Ignore ready that wake use after uninterruptible sleep, but follow
      -- ready if there is no previous state (thread is new).
      AND (prev_state != 'D' OR prev_state IS NULL)
      -- dur can be -1 when we're missing data, usually at the beginning and
      -- end of traces
      AND dur >= 0
  ),
  -- hardware_drm_waits are waits for hardware to be in a specific state. If we followed the
  -- thread_ready pass_work rows above we would end up at a dead end when we are woken by an
  -- interrupt. Instead we follow backwards to the beginning of the wait.
  hardware_drm_waits AS (
    SELECT
      ts AS ready_ts,
      utid AS ready_utid,
      ts + dur AS ts,
      utid,
      'hardware DRM wait' AS type,
      name AS info
    FROM thread_slice
    WHERE
      process_name = '/vendor/bin/hw/android.hardware.composer.hwc3-service.drm'
      AND is_main_thread = 1
      AND (name = 'EbusyRetryWait'
      OR name = 'WaitForPresentTime'
      OR name = 'WaitPriorFramePresented')
  ),
  -- monitor_lock_contention skip over regions where we are trying to acquire a lock. We don't want to
  -- follow path of the lock holder, because that is incidental to the actual critical path. Instead
  -- skip back to the beginning of the lock contention and keep following back on the same thread.
  monitor_lock_contention AS (
    SELECT
      ts AS ready_ts,
      utid AS ready_utid,
      ts + dur AS ts,
      utid,
      'lock contention' AS type,
      name AS info
    FROM thread_slice
    WHERE
      name GLOB 'Lock contention on a monitor lock*'
      AND dur > 0
  ),
  -- vsync follows Choreographer#onVsync slices back to the scheduleVsyncLocked call. If we did not,
  -- then we would reach a dead end when woken by the vsync timer interrupt.
  vsync_slices AS (
    SELECT
      ts,
      utid,
      name,
      lead(name) OVER (PARTITION BY utid ORDER BY ts ASC) AS next_name,
      lead(ts + dur) OVER (PARTITION BY utid ORDER BY ts ASC) AS next_end_ts
    FROM thread_slice
    WHERE
      name = 'Choreographer#scheduleVsyncLocked'
      OR (name GLOB 'Choreographer#onVsync *' AND dur >= 0)
  ),
  vsync AS (
    SELECT
      ts AS ready_ts,
      utid AS ready_utid,
      next_end_ts AS ts,
      utid,
      'vsync' AS type,
      substr(next_name, 23) AS info
    FROM vsync_slices
    WHERE
      name = 'Choreographer#scheduleVsyncLocked'
      AND next_name GLOB 'Choreographer#onVsync *'
  ),
  -- binder transactions follow both synchronous and asynchronous transactions.
  binder_sync_txns AS (
    SELECT
      tx.ts AS tx_ts,
      tx.dur AS tx_dur,
      tx.utid AS tx_utid,
      reply.ts AS reply_ts,
      reply.dur AS reply_dur,
      reply.utid AS reply_utid,
      CASE WHEN parent.name GLOB 'AIDL::*' THEN parent.name ELSE '' END AS info
    FROM thread_slice AS tx
    JOIN flow
      ON flow.slice_out = tx.id
    JOIN thread_slice AS reply
      ON reply.id = flow.slice_in
    LEFT JOIN slice AS parent
      ON parent.id = tx.parent_id
    WHERE
      tx.name = 'binder transaction'
      AND reply.name = 'binder reply'
  ),
  binder_sync_call AS (
    SELECT
      tx_ts AS ready_ts,
      tx_utid AS ready_utid,
      reply_ts AS ts,
      reply_utid AS utid,
      'binder transaction' AS type,
      info
    FROM binder_sync_txns
  ),
  binder_sync_reply AS (
    SELECT
      reply_ts + reply_dur AS ready_ts,
      reply_utid AS ready_utid,
      tx_ts + tx_dur AS ts,
      tx_utid AS utid,
      'binder reply' AS type,
      info
    FROM binder_sync_txns
    WHERE
      tx_dur >= 0
      AND reply_dur >= 0
  ),
  binder_async AS (
    SELECT
      tx.ts AS ready_ts,
      tx.utid AS ready_utid,
      rcv.ts AS ts,
      rcv.utid AS utid,
      'binder transaction async' AS type,
      CASE WHEN parent.name GLOB 'AIDL::*' THEN parent.name ELSE '' END AS info
    FROM thread_slice AS tx
    JOIN flow
      ON flow.slice_out = tx.id
    JOIN thread_slice AS rcv
      ON rcv.id = flow.slice_in
    LEFT JOIN slice AS parent
      ON parent.id = tx.parent_id
    WHERE
      tx.name = 'binder transaction async'
      AND rcv.name = 'binder async rcv'
  ),
  -- input_channel follows receiveMessage calls back the the corresponding sendMessage call.
  -- NB: Materializing ic_send prevents SQLite from flattening the CTE and performing a nested-loop
  -- scan of thread_slice for every ic_recv row, allowing SQLite to build an automatic index on
  -- message_id instead.
  ic_send AS MATERIALIZED (
    SELECT ts, utid, substr(name, 26, length(name) - 27) AS message_id
    FROM thread_slice
    WHERE
      name GLOB 'sendMessage(inputChannel=*'
  ),
  ic_recv AS (
    SELECT ts, utid, substr(name, 29, length(name) - 30) AS message_id
    FROM thread_slice
    WHERE
      name GLOB 'receiveMessage(inputChannel=*'
  ),
  input_channel AS (
    SELECT
      ic_send.ts AS ready_ts,
      ic_send.utid AS ready_utid,
      ic_recv.ts AS ts,
      ic_recv.utid AS utid,
      'input channel' AS type,
      message_id AS info
    FROM ic_recv
    JOIN ic_send USING (message_id)
  )
-- TODO: mojo
-- TODO: post task, looper, etc.
SELECT * FROM thread_ready
UNION ALL
SELECT * FROM hardware_drm_waits
UNION ALL
SELECT * FROM monitor_lock_contention
UNION ALL
SELECT * FROM vsync
UNION ALL
SELECT * FROM binder_sync_call
UNION ALL
SELECT * FROM binder_sync_reply
UNION ALL
SELECT * FROM binder_async
UNION ALL
SELECT * FROM input_channel;

-- Wakeup graph built from `_android_pass_work`.
CREATE PERFETTO TABLE _android_critical_path_graph AS
SELECT * FROM _critical_path_pass_work_graph!(_android_pass_work);

-- Critical path of the span `[ts, ts + dur]` ending on `end_utid`.
--
-- Starting from the thread active on `end_utid` at the end of the span, walks
-- backwards through the thread that passed it work (scheduler wakeups,
-- binder transactions and replies, vsync, input channel messages, ...) until
-- the start of the span. Walking backwards means parallel work never has to
-- be branched: only the work item that finished last and readied the next
-- thread is on the path.
--
-- ready_type and ready_info values:
-- * 'thread_ready': Woken from sleep via kernel scheduler wakeup (ready_utid is waker_utid, or NULL
--   if woken from interrupt context).
--   * '': Always empty.
-- * 'hardware DRM wait': HWC DRM main thread wait slice, passing from the start to the end of the
--   wait on the same thread.
--   * 'EbusyRetryWait': Waiting to retry after a DRM operation returned EBUSY.
--   * 'WaitForPresentTime': Waiting until the target presentation time arrives.
--   * 'WaitPriorFramePresented': Waiting for the prior frame to finish presenting.
-- * 'lock contention': Blocked waiting to acquire a Java monitor lock, passing work from the start
--   to the end of the contention slice on the same thread.
--   * 'Lock contention on a monitor lock*': Slice name, including the lock owner tid (e.g.
--     'Lock contention on a monitor lock (owner tid: 1234)').
-- * 'vsync': Waiting for a scheduled vsync callback, passing from the start of
--   'Choreographer#scheduleVsyncLocked' to the end of the next 'Choreographer#onVsync *' slice on
--   the same thread.
--   * '<vsync_id>': The vsync ID extracted from 'Choreographer#onVsync <vsync_id>'.
-- * 'binder transaction': Synchronous binder call from the client thread (ready_utid) at the start
--   of 'binder transaction' to the server thread (utid) at the start of 'binder reply'.
--   * 'AIDL::*': Name of the parent slice of 'binder transaction' if it starts with 'AIDL::'.
--   * '': Empty if there is no 'AIDL::*' parent slice.
-- * 'binder reply': Synchronous binder reply from the server thread (ready_utid) at the end of
--   'binder reply' back to the client thread (utid) at the end of 'binder transaction'.
--   * 'AIDL::*': Name of the parent slice of the corresponding 'binder transaction' if it starts
--     with 'AIDL::'.
--   * '': Empty if there is no 'AIDL::*' parent slice.
-- * 'binder transaction async': Asynchronous (one-way) binder call from the sender thread
--   (ready_utid) at 'binder transaction async' to the receiver thread (utid) at 'binder async rcv'.
--   * 'AIDL::*': Name of the parent slice of 'binder transaction async' if it starts with 'AIDL::'.
--   * '': Empty if there is no 'AIDL::*' parent slice.
-- * 'input channel': Input event sent over an InputChannel from the sender thread (ready_utid) at
--   'sendMessage(inputChannel=...)' to the receiver thread (utid) at
--   'receiveMessage(inputChannel=...)'.
--   * '<message_id>': The channel name, sequence number, and message type extracted from the slice
--     name.
CREATE PERFETTO FUNCTION android_critical_path(
  -- Start timestamp of the span.
  ts TIMESTAMP,
  -- Duration of the span.
  dur DURATION,
  -- Utid of the thread at the end of the span.
  end_utid JOINID(thread.id)
)
RETURNS TABLE(
  -- Hop index, 0 for the hop at the end of the span.
  depth LONG,
  -- Timestamp this thread was made ready (NULL if before the span).
  ready_ts TIMESTAMP,
  -- Timestamp this thread started the work (clipped to the span).
  ts TIMESTAMP,
  -- Duration until this thread passed work to the next hop.
  dur DURATION,
  -- Utid of the thread doing the work.
  utid JOINID(thread.id),
  -- Utid of the thread that made this thread ready.
  ready_utid JOINID(thread.id),
  -- Type of event that made this thread ready (see function docs).
  ready_type STRING,
  -- Additional info for ready_type (see function docs).
  ready_info STRING,
  -- Name of the thread.
  thread_name STRING,
  -- Name of the process of the thread.
  process_name STRING,
  -- Time spent in 'Running' state.
  running_dur DURATION,
  -- Time spent in 'R' or 'R+' (runnable / preempted) states.
  runnable_dur DURATION,
  -- Time spent in 'S' (interruptible sleep) state.
  sleeping_dur DURATION,
  -- Time spent in 'D' (uninterruptible sleep) state.
  uninterruptible_sleep_dur DURATION
)
AS
SELECT
  h.depth,
  h.ready_ts,
  h.ts,
  h.dur,
  h.utid,
  h.ready_utid,
  h.ready_type,
  h.ready_info,
  t.name AS thread_name,
  p.name AS process_name,
  h.running_dur,
  h.runnable_dur,
  h.sleeping_dur,
  h.uninterruptible_sleep_dur
FROM _critical_path_pass_work_hops!(
  (SELECT 0 AS id, $ts AS ts, $dur AS dur, $end_utid AS utid),
  _android_critical_path_graph
) AS h
JOIN thread AS t USING (utid)
LEFT JOIN process AS p USING (upid)
ORDER BY
  h.depth DESC;

-- Formats `android_critical_path` as intervals suitable for a Perfetto UI
-- debug track: one slice for the time each hop was waiting to be picked up
-- (`ready_type`) and one for the time it ran (`run: ...`).
CREATE PERFETTO FUNCTION android_critical_path_debug_track(
  -- Start timestamp of the span.
  ts TIMESTAMP,
  -- Duration of the span.
  dur DURATION,
  -- Utid of the thread at the end of the span.
  utid JOINID(thread.id)
)
RETURNS TABLE(
  -- Start timestamp of the debug track slice.
  ts TIMESTAMP,
  -- Duration of the debug track slice.
  dur DURATION,
  -- Display name of the debug track slice.
  name STRING,
  -- Color key for the debug track slice.
  color STRING,
  -- Time spent in 'Running' state in milliseconds.
  running_dur_ms DOUBLE,
  -- Time spent in 'R' or 'R+' states in milliseconds.
  runnable_dur_ms DOUBLE,
  -- Time spent in 'S' state in milliseconds.
  sleeping_dur_ms DOUBLE,
  -- Time spent in 'D' state in milliseconds.
  uninterruptible_sleep_dur_ms DOUBLE
)
AS
WITH path AS (SELECT * FROM android_critical_path($ts, $dur, $utid))
SELECT
  ts,
  dur,
  'run: ' || process_name || ' ' || thread_name AS name,
  process_name AS color,
  running_dur / 1e6 AS running_dur_ms,
  runnable_dur / 1e6 AS runnable_dur_ms,
  sleeping_dur / 1e6 AS sleeping_dur_ms,
  uninterruptible_sleep_dur / 1e6 AS uninterruptible_sleep_dur_ms
FROM path
WHERE
  dur > 0
UNION ALL
SELECT
  ready_ts AS ts,
  ts - ready_ts AS dur,
  ready_type || ' ' || ready_info AS name,
  ready_type AS color,
  NULL AS running_dur_ms,
  NULL AS runnable_dur_ms,
  NULL AS sleeping_dur_ms,
  NULL AS uninterruptible_sleep_dur_ms
FROM path
WHERE
  ready_ts IS NOT NULL;
