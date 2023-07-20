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

-- A 'thread_executing_span' is thread_state span starting with a runnable slice
-- until the next runnable slice that's woken up by a process (as opposed
-- to an interrupt). Note that within a 'thread_executing_span' we can have sleep
-- spans blocked on an interrupt.
-- We consider the id of this span to be the id of the first thread_state in the span.

--
-- Finds all runnable states that are woken up by a process.
--
-- We achieve this by checking that the |thread_state.irq_context|
-- value is NOT 1. In otherwords, it is either 0 or NULL. The NULL check
-- is important to support older Android versions.
--
-- On older versions of Android (<U). We don't have IRQ context information,
-- so this table might contain wakeups from interrupt context, consequently, the
-- wakeup graph generated might not be accurate.
--
CREATE VIEW internal_runnable_state
AS
SELECT
  thread_state.id,
  thread_state.ts,
  thread_state.dur,
  thread_state.state,
  thread_state.utid,
  thread_state.waker_utid
FROM thread_state
WHERE thread_state.dur != -1 AND thread_state.waker_utid IS NOT NULL
   AND (thread_state.irq_context = 0 OR thread_state.irq_context IS NULL);

-- Similar to |internal_runnable_state| but finds the runnable states at thread fork.
CREATE VIEW internal_fork_runnable_state
AS
SELECT
  thread_state.id,
  thread_state.ts,
  thread_state.dur,
  thread_state.state,
  thread_state.utid,
  thread_state.waker_utid,
  thread.start_ts = thread_state.ts AS is_fork
FROM thread_state
JOIN thread USING(utid)
WHERE thread_state.dur != -1 AND thread_state.waker_utid IS NOT NULL
   AND thread.start_ts = thread_state.ts;

--
-- Finds all sleep states including interruptible (S) and uninterruptible (D).
CREATE VIEW internal_sleep_state
AS
SELECT
  thread_state.id,
  thread_state.ts,
  thread_state.dur,
  thread_state.state,
  thread_state.blocked_function,
  thread_state.utid
FROM thread_state
WHERE dur != -1 AND (state = 'S' OR state = 'D');

--
-- Finds the last execution for every thread to end executing_spans without a Sleep.
--
CREATE VIEW internal_thread_end_ts
AS
SELECT
  MAX(ts) + dur AS end_ts,
  utid
FROM thread_state
WHERE dur != -1
GROUP BY utid;

-- Similar to |internal_sleep_state| but finds the first sleep state after thread fork.
CREATE VIEW internal_fork_sleep_state
AS
SELECT
  MIN(thread_state.id) AS id,
  thread_state.ts,
  thread_state.dur,
  thread_state.state,
  thread_state.blocked_function,
  thread_state.utid
FROM thread_state
WHERE dur != -1 AND (state = 'S' OR state = 'D')
GROUP BY utid;

--
-- Finds all neighbouring ('Sleeping', 'Runnable') thread_states pairs from the same thread.
-- More succintly, pairs of S[n-1]-R[n] where R is woken by a process context and S is an
-- interruptible or uninterruptible sleep state.
--
-- This is achieved by joining the |internal_runnable_state|.ts with the
-- |internal_sleep_state|.|ts + dur|.
--
-- With the S-R pairs of a thread, we can re-align to [R-S) intervals with LEADS and LAGS.
--
-- Given the following thread_states on a thread:
-- S0__|R0__Running0___|S1__|R1__Running1___|S2__|R2__Running2__S2|.
--
-- We have 3 thread_executing_spans: [R0, S0), [R1, S1), [R2, S2).
--
-- We define the following markers in this table:
--
-- prev_start_id    = R0_id.
-- prev_start_ts    = R0_ts.
-- prev_start_dur   = R0_dur.
-- prev_start_state = 'R'.
--
-- prev_end_id      = S0_id.
-- prev_end_ts      = S0_ts.
-- prev_end_dur     = S0_dur.
-- prev_end_state   = 'S' or 'D'.
--
-- start_id         = R1_id.
-- start_ts         = R1_ts.
-- start_dur        = R1_dur.
-- start_state      = 'R'.
--
-- end_id           = S1_id.
-- end_ts           = S1_ts.
-- end_dur          = S1_dur.
-- end_state        = 'S' or 'D'.
CREATE TABLE internal_wakeup
AS
  SELECT
  LAG(r.id, 1) OVER (PARTITION BY r.utid ORDER BY r.ts) AS prev_start_id,
  LAG(r.ts, 1) OVER (PARTITION BY r.utid ORDER BY r.ts) AS prev_start_ts,
  LAG(r.dur, 1) OVER (PARTITION BY r.utid ORDER BY r.ts) AS prev_start_dur,
  LAG(r.state, 1) OVER (PARTITION BY r.utid ORDER BY r.ts) AS prev_start_state,
  s.id AS prev_end_id,
  s.ts AS prev_end_ts,
  s.dur AS prev_end_dur,
  s.state AS prev_end_state,
  s.blocked_function AS prev_blocked_function,
  r.id AS start_id,
  r.ts AS start_ts,
  r.dur AS start_dur,
  r.state AS start_state,
  r.utid AS utid,
  r.waker_utid,
  0 AS is_fork,
  LEAD(s.id, 1) OVER (PARTITION BY r.utid ORDER BY r.ts) AS end_id,
  IFNULL(LEAD(s.ts, 1) OVER (PARTITION BY r.utid ORDER BY r.ts), thread_end.end_ts)  AS end_ts,
  LEAD(s.dur, 1) OVER (PARTITION BY r.utid ORDER BY r.ts) AS end_dur,
  LEAD(s.state, 1) OVER (PARTITION BY r.utid ORDER BY r.ts) AS end_state,
  LEAD(s.blocked_function, 1) OVER (PARTITION BY r.utid ORDER BY r.ts) AS blocked_function
FROM internal_runnable_state r
JOIN internal_sleep_state s
  ON s.utid = r.utid AND (s.ts + s.dur = r.ts)
LEFT JOIN internal_thread_end_ts thread_end USING(utid)
UNION ALL
  SELECT
  NULL AS prev_start_id,
  NULL AS prev_start_ts,
  NULL AS prev_start_dur,
  NULL AS prev_start_state,
  NULL AS prev_end_id,
  NULL AS prev_end_ts,
  NULL AS prev_end_dur,
  NULL AS prev_end_state,
  NULL AS prev_blocked_function,
  r.id AS start_id,
  r.ts AS start_ts,
  r.dur AS start_dur,
  r.state AS start_state,
  r.utid AS utid,
  r.waker_utid,
  r.is_fork,
  s.id AS end_id,
  s.ts AS end_ts,
  s.dur AS end_dur,
  s.state AS end_state,
  s.blocked_function AS blocked_function
FROM internal_fork_runnable_state r
JOIN internal_fork_sleep_state s
  ON s.utid = r.utid;

-- Improves performance of |internal_wakeup_chain| computation.
CREATE
  INDEX internal_wakeup_idx
ON internal_wakeup(waker_utid, start_ts);

--
-- Builds the parent-child chain from all thread_executing_spans. The parent is the waker and
-- child is the wakee.
--
-- Note that this doesn't include the roots. We'll compute the roots below.
-- This two step process improves performance because it's more efficient to scan
-- parent and find a child between than to scan child and find the parent it lies between.
CREATE VIEW internal_wakeup_chain
AS
SELECT parent.start_id AS parent_id, child.*
FROM internal_wakeup parent
JOIN internal_wakeup child
  ON (
    parent.utid = child.waker_utid
    AND child.start_ts BETWEEN parent.start_ts AND parent.end_ts);

--
-- Finds the roots of the |internal_wakeup_chain|.
CREATE VIEW internal_wakeup_root
AS
WITH
  internal_wakeup_root_id AS (
    SELECT DISTINCT parent_id AS id FROM internal_wakeup_chain
    EXCEPT
    SELECT DISTINCT start_id AS id FROM internal_wakeup_chain
  )
SELECT NULL AS parent_id, internal_wakeup.*
FROM internal_wakeup
JOIN internal_wakeup_root_id
  ON internal_wakeup_root_id.id = internal_wakeup.start_id;

CREATE TABLE internal_wakeup_leaf AS
WITH
  internal_wakeup_leaf_id AS (
    SELECT DISTINCT start_id AS id FROM internal_wakeup_chain
    EXCEPT
    SELECT DISTINCT parent_id AS id FROM internal_wakeup_chain
  )
SELECT internal_wakeup_chain.*
FROM internal_wakeup_chain
JOIN internal_wakeup_leaf_id
  ON internal_wakeup_leaf_id.id = internal_wakeup_chain.start_id;

--
-- Merges the roots and the rest of the chain.
CREATE TABLE internal_wakeup_graph
AS
SELECT internal_wakeup_chain.*, 0 AS is_root, (internal_wakeup_leaf.start_id IS NOT NULL) AS is_leaf
FROM internal_wakeup_chain
LEFT JOIN internal_wakeup_leaf
  USING (start_id)
UNION ALL
SELECT *, 1 AS is_root, 0 AS is_leaf FROM internal_wakeup_root;

-- thread_executing_span graph of all wakeups across all processes.
--
-- @column parent_id          Id of thread_executing_span that directly woke |id|
-- @column id                 Id of the first (runnable) thread state in thread_executing_span.
-- @column ts                 Timestamp of first thread_state in thread_executing_span.
-- @column dur                Duration of thread_executing_span.
-- @column tid                Tid of thread with thread_state.
-- @column pid                Pid of process with thread_state.
-- @column utid               Utid of thread with thread_state.
-- @column upid               Upid of process with thread_state.
-- @column thread_name        Name of thread with thread_state.
-- @column process_name       Name of process with thread_state.
-- @column waker_tid          Tid of thread that woke the first thread_state in thread_executing_span.
-- @column waker_pid          Pid of process that woke the first thread_state in thread_executing_span.
-- @column waker_utid         Utid of thread that woke the first thread_state in thread_executing_span.
-- @column waker_upid         Upid of process that woke the first thread_state in thread_executing_span.
-- @column waker_thread_name  Name of thread that woke the first thread_state in thread_executing_span.
-- @column waker_process_name Name of process that woke the first thread_state in thread_executing_span.
-- @column blocked_dur        Duration of blocking thread state before waking up.
-- @column blocked_state      Thread state ('D' or 'S') of blocked thread_state before waking up.
-- @column blocked_function   Kernel blocking function of thread state before waking up.
CREATE TABLE experimental_thread_executing_span_graph
AS
SELECT
  graph.parent_id,
  graph.start_id AS id,
  graph.start_ts AS ts,
  graph.end_ts - graph.start_ts AS dur,
  thread.tid,
  process.pid,
  graph.utid,
  process.upid,
  thread.name AS thread_name,
  process.name AS process_name,
  waker_thread.tid AS waker_tid,
  waker_process.pid AS waker_pid,
  graph.waker_utid,
  waker_process.upid AS waker_upid,
  waker_thread.name AS waker_thread_name,
  waker_process.name AS waker_process_name,
  graph.prev_end_dur AS blocked_dur,
  graph.prev_end_state AS blocked_state,
  graph.prev_blocked_function AS blocked_function,
  graph.is_root,
  graph.is_leaf
FROM internal_wakeup_graph graph
JOIN thread
  ON thread.utid = graph.utid
LEFT JOIN process
  ON process.upid = thread.upid
LEFT JOIN thread waker_thread
  ON waker_thread.utid = graph.waker_utid
LEFT JOIN process waker_process
  ON waker_process.upid = waker_thread.upid;

CREATE
  INDEX experimental_thread_executing_span_graph_id_idx
  ON experimental_thread_executing_span_graph(id);

CREATE
  INDEX experimental_thread_executing_span_graph_parent_id_idx
ON experimental_thread_executing_span_graph(parent_id);


-- All thread_executing_spans that were recursively woken by |root_id|, all thread_executing_spans
-- in trace.
-- if root_id IS NULL, empty results if no matching thread_state id found.
--
-- @arg root_id INT           Thread state id to start recursion
--
-- @column parent_id          Id of thread_executing_span that directly woke |start_id|
-- @column id                 Id of the first (runnable) thread state in thread_executing_span.
-- @column ts                 Timestamp of first thread_state in thread_executing_span.
-- @column dur                Duration of thread_executing_span.
-- @column tid                Tid of thread with thread_state.
-- @column pid                Pid of process with thread_state.
-- @column utid               Utid of thread with thread_state.
-- @column upid               Upid of process with thread_state.
-- @column thread_name        Name of thread with thread_state.
-- @column process_name       Name of process with thread_state.
-- @column waker_tid          Tid of thread that woke the first thread_state in thread_executing_span.
-- @column waker_pid          Pid of process that woke the first thread_state in thread_executing_span.
-- @column waker_utid         Utid of thread that woke the first thread_state in thread_executing_span.
-- @column waker_upid         Upid of process that woke the first thread_state in thread_executing_span.
-- @column waker_thread_name  Name of thread that woke the first thread_state in thread_executing_span.
-- @column waker_process_name Name of process that woke the first thread_state in thread_executing_span.
-- @column blocked_dur        Duration of blocking thread state before waking up.
-- @column blocked_state      Thread state ('D' or 'S') of blocked thread_state before waking up.
-- @column blocked_function   Kernel blocking function of thread state before waking up.
-- @column is_root            Whether this span is the root in the slice tree.
-- @column is_leaf            Whether this span is the leaf in the slice tree.
-- @column depth              Tree depth from |root_id|
-- @column root_id            Thread state id used to start the recursion. Helpful for SQL JOINs
SELECT CREATE_VIEW_FUNCTION(
'EXPERIMENTAL_THREAD_EXECUTING_SPAN_DESCENDANTS(root_id INT)',
'
  parent_id LONG,
  id LONG,
  ts LONG,
  dur LONG,
  tid INT,
  pid INT,
  utid INT,
  upid INT,
  thread_name STRING,
  process_name STRING,
  waker_tid INT,
  waker_pid INT,
  waker_utid INT,
  waker_upid INT,
  waker_thread_name STRING,
  waker_process_name STRING,
  blocked_dur LONG,
  blocked_state STRING,
  blocked_function STRING,
  is_root INT,
  is_leaf INT,
  depth INT,
  root_id INT
',
'
WITH chain AS (
  SELECT
    *,
    0 AS depth,
    id AS root_id
  FROM experimental_thread_executing_span_graph
  WHERE ($root_id IS NOT NULL AND id = $root_id) OR ($root_id IS NULL AND is_root)
  UNION ALL
  SELECT
    graph.*,
    chain.depth + 1 AS depth,
    chain.root_id
  FROM experimental_thread_executing_span_graph graph
  JOIN chain ON chain.id = graph.parent_id
)
SELECT * FROM chain
');

-- All thread_executing_spans that are ancestors of |leaf_id|.
--
-- @arg leaf_id INT                Thread state id to start recursion.
--
-- @column parent_id               Id of thread_executing_span that directly woke |id|.
-- @column id                      Id of the first (runnable) thread state in thread_executing_span.
-- @column ts                      Timestamp of first thread_state in thread_executing_span.
-- @column dur                     Duration of thread_executing_span.
-- @column tid                     Tid of thread with thread_state.
-- @column pid                     Pid of process with thread_state.
-- @column utid                    Utid of thread with thread_state.
-- @column upid                    Upid of process with thread_state.
-- @column thread_name             Name of thread with thread_state.
-- @column process_name            Name of process with thread_state.
-- @column waker_tid               Tid of thread that woke the first thread_state in thread_executing_span.
-- @column waker_pid               Pid of process that woke the first thread_state in thread_executing_span.
-- @column waker_utid              Utid of thread that woke the first thread_state in thread_executing_span.
-- @column waker_upid              Upid of process that woke the first thread_state in thread_executing_span.
-- @column waker_thread_name       Name of thread that woke the first thread_state in thread_executing_span.
-- @column waker_process_name      Name of process that woke the first thread_state in thread_executing_span.
-- @column blocked_dur             Duration of blocking thread state before waking up.
-- @column blocked_state           Thread state ('D' or 'S') of blocked thread_state before waking up.
-- @column blocked_function        Kernel blocking function of thread state before waking up.
-- @column is_root                 Whether this span is the root in the slice tree.
-- @column is_leaf                 Whether this span is the leaf in the slice tree.
-- @column height                  Tree height from |leaf_id|.
-- @column leaf_id                 Thread state id used to start the recursion. Helpful for SQL JOINs.
-- @column leaf_ts                 Thread state timestamp of the |leaf_id|.
-- @column leaf_blocked_dur        Thread state duration blocked of the |leaf_id|.
-- @column leaf_blocked_state      Thread state of the |leaf_id|.
-- @column leaf_blocked_function   Thread state blocked_function of the |leaf_id|.
SELECT CREATE_VIEW_FUNCTION(
'EXPERIMENTAL_THREAD_EXECUTING_SPAN_ANCESTORS(leaf_id INT)',
'
  parent_id LONG,
  id LONG,
  ts LONG,
  dur LONG,
  tid INT,
  pid INT,
  utid INT,
  upid INT,
  thread_name STRING,
  process_name STRING,
  waker_tid INT,
  waker_pid INT,
  waker_utid INT,
  waker_upid INT,
  waker_thread_name STRING,
  waker_process_name STRING,
  blocked_dur LONG,
  blocked_state STRING,
  blocked_function STRING,
  is_root INT,
  is_leaf INT,
  height INT,
  leaf_id INT,
  leaf_ts LONG,
  leaf_blocked_dur LONG,
  leaf_blocked_state STRING,
  leaf_blocked_function STRING
',
'
WITH
chain AS (
  SELECT
    *,
    0 AS height,
    id AS leaf_id,
    ts AS leaf_ts,
    blocked_dur AS leaf_blocked_dur,
    blocked_state AS leaf_blocked_state,
    blocked_function AS leaf_blocked_function
  FROM experimental_thread_executing_span_graph
  WHERE ($leaf_id IS NOT NULL AND id = $leaf_id) OR ($leaf_id IS NULL AND is_leaf)
  UNION ALL
  SELECT
    graph.*,
    chain.height + 1 AS height,
    chain.leaf_id,
    chain.leaf_ts,
    chain.leaf_blocked_dur,
    chain.leaf_blocked_state,
    chain.leaf_blocked_function
  FROM experimental_thread_executing_span_graph graph
  JOIN chain ON chain.parent_id = graph.id AND chain.ts >= (leaf_ts - leaf_blocked_dur)
)
SELECT * FROM chain
');

-- Gets the thread_executing_span id a thread_state belongs to. Returns NULL if thread state is
-- sleeping and not blocked on an interrupt.
--
-- @arg thread_state_id INT   Id of the thread_state to get the thread_executing_span id for
-- @ret INT                   thread_executing_span id
CREATE PERFETTO FUNCTION
EXPERIMENTAL_THREAD_EXECUTING_SPAN_ID_FROM_THREAD_STATE_ID(thread_state_id INT)
RETURNS INT AS
WITH t AS (
  SELECT
    ts,
    utid
  FROM thread_state
  WHERE
    id = $thread_state_id
)
SELECT
  MAX(start_id) AS thread_executing_span_id
FROM internal_wakeup w, t
WHERE t.utid = w.utid AND t.ts >= w.start_ts AND t.ts < w.end_ts;
