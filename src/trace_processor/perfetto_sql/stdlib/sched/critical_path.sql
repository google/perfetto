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

INCLUDE PERFETTO MODULE slices.with_context;

CREATE PERFETTO TABLE _pass_work(
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

CREATE PERFETTO INDEX _pass_work_index ON _pass_work(utid, ts);

-- _critical_path_prev finds the previous step in the critical path by looking for the most recent
-- _pass_work row on a given thread.
CREATE PERFETTO FUNCTION _critical_path_prev(
  ts TIMESTAMP,
  utid JOINID(thread.id),
  end_ts TIMESTAMP
)
RETURNS TABLE(
  ready_ts TIMESTAMP,
  ts TIMESTAMP,
  dur DURATION,
  utid JOINID(thread.id),
  ready_utid JOINID(thread.id),
  ready_type STRING,
  ready_info STRING
)
AS
SELECT
  CASE WHEN ready_ts <= $ts THEN NULL ELSE ready_ts END AS ready_ts,
  max($ts, ts) AS ts,
  $end_ts - max($ts, ts) AS dur,
  utid,
  ready_utid,
  type AS ready_type,
  info AS ready_info
FROM _pass_work
WHERE
  utid = $utid
  AND ts <= $end_ts
--AND ts + dur > $ts
ORDER BY
  ts DESC
LIMIT 1;

-- Builds a path of thread activity by starting at the end of a span and working
-- backwards to the start following RPCs and thread_ready events. The advantage of working backwards
-- is that we don't need to branch the path if there are multiple parallel work items that need to
-- finish before the end of the span. We follow the one that finished last and ended up waking the
-- thread that completed the work. So reducing cost on the critical path by X ms may not improve
-- responsiveness by the same amount.
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
CREATE PERFETTO FUNCTION sched_critical_path(
  -- Start timestamp of the span.
  ts TIMESTAMP,
  -- Duration of the span.
  dur DURATION,
  -- Utid of the thread at the end of the span.
  end_utid JOINID(thread.id)
)
RETURNS TABLE(
  -- Ready timestamp, same as the previous thread's ts + dur.
  ready_ts TIMESTAMP,
  -- Start timestamp of the thread after ready.
  ts TIMESTAMP,
  -- Thread run duration until the next ready.
  dur DURATION,
  -- Utid of the running thread.
  utid JOINID(thread.id),
  -- Utid of the thread that readied this thread.
  ready_utid JOINID(thread.id),
  -- Type of event that readied this thread (see function docs).
  ready_type STRING,
  -- Additional info for ready_type (see function docs).
  ready_info STRING,
  -- Name of the running thread.
  thread_name STRING,
  -- Name of the process of the running thread.
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
WITH RECURSIVE
  path AS (
    SELECT * FROM _critical_path_prev($ts, $end_utid, $ts + $dur)
    UNION ALL
    SELECT prev.*
    FROM path AS next
    -- NB: next.ready_utid is NULL for wakes from interrupts.
    JOIN _critical_path_prev($ts, next.ready_utid, next.ready_ts) AS prev
  ),
  -- Assign a unique step_id to each critical path step. We use the step_id to
  -- reaggregate back to one row per step after joining thread state data in.
  path_with_step_id AS (
    SELECT
      row_number() OVER (ORDER BY p.ts ASC) AS step_id,
      p.*,
      t.name AS thread_name,
      proc.name AS process_name
    FROM path AS p
    JOIN thread AS t USING (utid)
    JOIN process AS proc USING (upid)
  ),
  thread_state_spans AS (
    SELECT
      p.step_id,
      s.state,
      min(
        p.ts + p.dur,
        CASE WHEN s.dur = -1 THEN p.ts + p.dur ELSE s.ts + s.dur END
      )
      - max(p.ts, s.ts) AS overlap_dur
    FROM path_with_step_id AS p
    JOIN thread_state AS s
      ON s.utid = p.utid
      AND s.ts < p.ts + p.dur
      AND (s.dur = -1 OR s.ts + s.dur > p.ts)
      AND p.dur > 0
  ),
  aggregated_states AS (
    SELECT
      step_id,
      sum(CASE WHEN state = 'Running' THEN overlap_dur ELSE 0 END) AS running_dur,
      sum(CASE WHEN state IN ('R', 'R+') THEN overlap_dur ELSE 0 END) AS runnable_dur,
      sum(CASE WHEN state = 'S' THEN overlap_dur ELSE 0 END) AS sleeping_dur,
      sum(CASE WHEN state = 'D' THEN overlap_dur ELSE 0 END) AS uninterruptible_sleep_dur
    FROM thread_state_spans
    GROUP BY
      step_id
  )
SELECT
  p.ready_ts,
  p.ts,
  p.dur,
  p.utid,
  p.ready_utid,
  p.ready_type,
  p.ready_info,
  p.thread_name,
  p.process_name,
  coalesce(a.running_dur, 0) AS running_dur,
  coalesce(a.runnable_dur, 0) AS runnable_dur,
  coalesce(a.sleeping_dur, 0) AS sleeping_dur,
  coalesce(a.uninterruptible_sleep_dur, 0) AS uninterruptible_sleep_dur
FROM path_with_step_id AS p
LEFT JOIN aggregated_states AS a USING (step_id)
ORDER BY
  p.ts ASC;

-- Formats the critical path of a time span [ts, ts + dur] ending on `utid`
-- as intervals suitable for a Perfetto UI debug track.
CREATE PERFETTO FUNCTION sched_critical_path_debug_track(
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
WITH path AS (SELECT * FROM sched_critical_path($ts, $dur, $utid))
SELECT
  ts,
  dur,
  'run: ' || process_name || ' ' || thread_name AS name,
  process_name AS color,
  CAST(path.running_dur AS DOUBLE) / 1000000.0 AS running_dur_ms,
  CAST(path.runnable_dur AS DOUBLE) / 1000000.0 AS runnable_dur_ms,
  CAST(path.sleeping_dur AS DOUBLE) / 1000000.0 AS sleeping_dur_ms,
  CAST(path.uninterruptible_sleep_dur AS DOUBLE) / 1000000.0 AS uninterruptible_sleep_dur_ms
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
