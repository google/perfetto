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
SELECT IMPORT('common.slices');

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

-- Similar to |internal_runnable_state| but finds the first runnable state at thread.
CREATE VIEW internal_first_runnable_state
AS
WITH
  first_state AS (
    SELECT
      MIN(thread_state.id) AS id
    FROM thread_state
    GROUP BY utid
  )
SELECT
  thread_state.id,
  thread_state.ts,
  thread_state.dur,
  thread_state.state,
  thread_state.utid,
  thread_state.waker_utid
FROM thread_state
JOIN first_state USING (id)
WHERE thread_state.dur != -1 AND thread_state.state = 'R';

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
WHERE dur != -1 AND (state = 'S' OR state = 'D' OR state = 'I');

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

-- Similar to |internal_sleep_state| but finds the first sleep state in a thread.
CREATE VIEW internal_first_sleep_state
AS
SELECT
  MIN(s.id) AS id,
  s.ts,
  s.dur,
  s.state,
  s.blocked_function,
  s.utid
FROM internal_sleep_state s
JOIN internal_runnable_state r
  ON s.utid = r.utid AND (s.ts + s.dur = r.ts)
GROUP BY s.utid;

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
  s.id AS end_id,
  IFNULL(s.ts, thread_end.end_ts)  AS end_ts,
  s.dur AS end_dur,
  s.state AS end_state,
  s.blocked_function AS blocked_function
FROM internal_first_runnable_state r
LEFT JOIN internal_first_sleep_state s
  ON s.utid = r.utid
LEFT JOIN internal_thread_end_ts thread_end USING(utid);

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

CREATE PERFETTO TABLE internal_wakeup_leaf AS
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
CREATE PERFETTO TABLE internal_wakeup_graph
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
-- @arg leaf_utid INT              Thread utid to start recursion from.
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
-- @column leaf_utid               Thread Utid of the |leaf_id|.
-- @column leaf_blocked_dur        Thread state duration blocked of the |leaf_id|.
-- @column leaf_blocked_state      Thread state of the |leaf_id|.
-- @column leaf_blocked_function   Thread state blocked_function of the |leaf_id|.
SELECT CREATE_VIEW_FUNCTION(
'EXPERIMENTAL_THREAD_EXECUTING_SPAN_ANCESTORS(leaf_id INT, leaf_utid INT)',
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
  leaf_utid INT,
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
    utid AS leaf_utid,
    blocked_dur AS leaf_blocked_dur,
    blocked_state AS leaf_blocked_state,
    blocked_function AS leaf_blocked_function
  FROM experimental_thread_executing_span_graph
  WHERE (($leaf_id IS NOT NULL AND id = $leaf_id) OR ($leaf_id IS NULL))
    AND (($leaf_utid IS NOT NULL AND utid = $leaf_utid) OR ($leaf_utid IS NULL))
  UNION ALL
  SELECT
    graph.*,
    chain.height + 1 AS height,
    chain.leaf_id,
    chain.leaf_ts,
    chain.leaf_utid,
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
experimental_thread_executing_span_id_from_thread_state_id(thread_state_id INT)
RETURNS INT AS
WITH executing AS (
  SELECT
    ts,
    utid
  FROM thread_state
  WHERE
    id = $thread_state_id
)
SELECT
  MAX(start_id) AS thread_executing_span_id
FROM internal_wakeup wakeup, executing
WHERE executing.utid = wakeup.utid AND executing.ts >= wakeup.start_ts AND executing.ts < wakeup.end_ts;

-- Gets the next thread_executing_span id after a sleeping state. Returns NULL if there is no
-- thread_executing_span after the |thread_state_id|.
--
-- @arg thread_state_id INT   Id of the thread_state to get the next thread_executing_span id for
-- @ret INT                   thread_executing_span id
CREATE PERFETTO FUNCTION
experimental_thread_executing_span_following_thread_state_id(thread_state_id INT)
RETURNS INT AS
WITH
  sleeping AS (
  SELECT
    ts,
    utid
  FROM thread_state
  WHERE
    id = $thread_state_id AND (state = 'S' OR state = 'D' OR state = 'I')
  )
SELECT MIN(start_id) AS thread_executing_span_id
FROM internal_wakeup wakeup, sleeping
WHERE sleeping.utid = wakeup.utid AND sleeping.ts < wakeup.start_ts;

-- Computes the start of each thread_executing_span in the critical path.

-- It finds the MAX between the start of the critical span and the start
-- of the blocked region. This ensures that the critical path doesn't overlap
-- the preceding thread_executing_span before the blocked region.
CREATE PERFETTO FUNCTION internal_critical_path_start_ts(ts LONG, leaf_ts LONG, leaf_blocked_dur LONG)
RETURNS LONG AS SELECT MAX($ts, IFNULL($leaf_ts - $leaf_blocked_dur, $ts));

-- Critical path of thread_executing_spans blocking the thread_executing_span with id,
-- |thread_executing_span_id|. For a given thread state span, its duration in the critical path
-- is the range between the start of the thread_executing_span and the start of the next span in the
-- critical path.
--
-- @arg thread_executing_span_id INT        Id of blocked thread_executing_span.
-- @arg utid INT                            Thread utid to pre-filter critical paths for.
--
-- @column parent_id                        Id of thread_executing_span that directly woke |id|.
-- @column id                               Id of the first (runnable) thread state in thread_executing_span.
-- @column ts                               Timestamp of first thread_state in thread_executing_span.
-- @column dur                              Duration of thread_executing_span within the critical path.
-- @column tid                              Tid of thread with thread_state.
-- @column pid                              Pid of process with thread_state.
-- @column utid                             Utid of thread with thread_state.
-- @column upid                             Upid of process with thread_state.
-- @column thread_name                      Name of thread with thread_state.
-- @column process_name                     Name of process with thread_state.
-- @column waker_tid                        Tid of thread that woke the first thread_state in thread_executing_span.
-- @column waker_pid                        Pid of process that woke the first thread_state in thread_executing_span.
-- @column waker_utid                       Utid of thread that woke the first thread_state in thread_executing_span.
-- @column waker_upid                       Upid of process that woke the first thread_state in thread_executing_span.
-- @column waker_thread_name                Name of thread that woke the first thread_state in thread_executing_span.
-- @column waker_process_name               Name of process that woke the first thread_state in thread_executing_span.
-- @column blocked_dur                      Duration of blocking thread state before waking up.
-- @column blocked_state                    Thread state ('D' or 'S') of blocked thread_state before waking up.
-- @column blocked_function                 Kernel blocking function of thread state before waking up.
-- @column is_root                          Whether this span is the root in the slice tree.
-- @column is_leaf                          Whether this span is the leaf in the slice tree.
-- @column height                           Tree height from |leaf_id|.
-- @column leaf_id                          Thread state id used to start the recursion. Helpful for SQL JOINs.
-- @column leaf_ts                          Thread state timestamp of the |leaf_id|.
-- @column leaf_utid                        Thread Utid of the |leaf_id|. Helpful for post-filtering the critical path to those originating from a thread.
-- @column leaf_blocked_dur                 Thread state duration blocked of the |leaf_id|.
-- @column leaf_blocked_state               Thread state of the |leaf_id|.
-- @column leaf_blocked_function            Thread state blocked_function of the |leaf_id|.
CREATE PERFETTO FUNCTION experimental_thread_executing_span_critical_path(thread_executing_span_id INT, leaf_utid INT)
RETURNS TABLE(
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
  leaf_utid INT,
  leaf_blocked_dur LONG,
  leaf_blocked_state STRING,
  leaf_blocked_function STRING
) AS
 SELECT
    parent_id,
    id,
    -- Here we compute the real ts and dur of a span in the critical_path.
    -- For the ts, we simply use the internal_critical_path_start_ts helper function.
    internal_critical_path_start_ts(ts, leaf_ts, leaf_blocked_dur) AS ts,
    -- For the dur, it is the MIN between the end of the current span and the start of
    -- the next span in the critical path. This ensures that the critical paths don't overlap.
    -- We are simply doing a MIN(real_ts + dur, lead(ts) - ts). It's written inline here for
    -- performance reasons. Note that we don't need the real_ts in the lead() because the real_ts
    -- is really only needed for the first ts in the critical path.
    MIN(internal_critical_path_start_ts(ts, leaf_ts, leaf_blocked_dur) + dur,
        IFNULL(LEAD(ts) OVER (PARTITION BY leaf_id ORDER BY height DESC), trace_bounds.end_ts))
    - internal_critical_path_start_ts(ts, leaf_ts, leaf_blocked_dur) AS dur,
    tid,
    pid,
    utid,
    upid,
    thread_name,
    process_name,
    waker_tid,
    waker_pid,
    waker_utid,
    waker_upid,
    waker_thread_name,
    waker_process_name,
    blocked_dur,
    blocked_state,
    blocked_function,
    is_root,
    is_leaf,
    height,
    leaf_id,
    leaf_ts,
    leaf_utid,
    leaf_blocked_dur,
    leaf_blocked_state,
    leaf_blocked_function
  FROM experimental_thread_executing_span_ancestors($thread_executing_span_id, $leaf_utid),
    trace_bounds;

-- Critical path of thread_executing_spans 'span joined' with their thread states.
-- See |experimental_thread_executing_span_critical_path|.
--
-- @arg leaf_utid INT                       Thread utid to filter critical paths for.
--
-- @column id                               Id of the first (runnable) thread state in thread_executing_span.
-- @column thread_state_id                  Id of thread_state in the critical path.
-- @column ts                               Timestamp of thread_state in the critical path.
-- @column dur                              Duration of thread_state in the critical path.
-- @column tid                              Tid of thread with thread_state.
-- @column pid                              Pid of process with thread_state.
-- @column utid                             Utid of thread with thread_state.
-- @column upid                             Upid of process with thread_state.
-- @column thread_name                      Name of thread with thread_state.
-- @column process_name                     Name of process with thread_state.
-- @column state                            Thread state of thread in the critical path.
-- @column blocked_function                 Blocked function of thread in the critical path.
-- @column height                           Tree height of thread_executing_span thread_state belongs to.
-- @column leaf_utid                        Thread Utid the critical path was filtered to.
CREATE PERFETTO FUNCTION experimental_thread_executing_span_critical_path_thread_states(leaf_utid INT)
RETURNS TABLE(
  id INT,
  thread_state_id INT,
  ts LONG,
  dur LONG,
  tid INT,
  pid INT,
  utid INT,
  upid INT,
  thread_name STRING,
  process_name STRING,
  state STRING,
  blocked_function STRING,
  height INT,
  leaf_utid INT
) AS
WITH
  span_starts AS (
    SELECT
      span.id,
      thread_state.id AS thread_state_id,
      MAX(thread_state.ts, span.ts) AS ts,
      span.ts + span.dur AS span_end_ts,
      thread_state.ts + thread_state.dur AS thread_state_end_ts,
      span.tid,
      span.pid,
      span.utid,
      span.upid,
      span.thread_name,
      span.process_name,
      thread_state.state,
      thread_state.blocked_function,
      span.height,
      span.leaf_utid
    FROM experimental_thread_executing_span_critical_path(NULL, $leaf_utid) span
    JOIN thread_state
      ON
        thread_state.utid = span.utid
        AND ((thread_state.ts BETWEEN span.ts AND span.ts + span.dur)
             OR (span.ts BETWEEN thread_state.ts AND thread_state.ts + thread_state.dur))
  )
SELECT
  id,
  thread_state_id,
  ts,
  MIN(span_end_ts, thread_state_end_ts) - ts AS dur,
  tid,
  pid,
  utid,
  upid,
  thread_name,
  process_name,
  state,
  blocked_function,
  height,
  leaf_utid
FROM span_starts
WHERE MIN(span_end_ts, thread_state_end_ts) - ts > 0;

-- Critical path of thread_executing_spans 'span joined' with their slices.
-- See |experimental_thread_executing_span_critical_path|.
--
-- @arg leaf_utid INT                       Thread utid to filter critical paths for.
--
-- @column id                               Id of the first (runnable) thread state in thread_executing_span.
-- @column slice_id                         Id of slice in the critical path.
-- @column ts                               Timestamp of slice in the critical path.
-- @column dur                              Duration of slice in the critical path.
-- @column tid                              Tid of thread that emitted the slice.
-- @column pid                              Pid of process that emitted the slice.
-- @column utid                             Utid of thread that emitted the slice.
-- @column upid                             Upid of process that emitted the slice.
-- @column thread_name                      Name of thread that emitted the slice.
-- @column process_name                     Name of process that emitted the slice.
-- @column slice_name                       Name of slice in the critical path.
-- @column slice_depth                      Depth of slice in its slice stack in the critical path.
-- @column height                           Tree height of thread_executing_span the slice belongs to.
-- @column leaf_utid                        Thread Utid the critical path was filtered to.
CREATE PERFETTO FUNCTION experimental_thread_executing_span_critical_path_slices(leaf_utid INT)
RETURNS TABLE(
  id INT,
  slice_id INT,
  ts LONG,
  dur LONG,
  tid INT,
  pid INT,
  utid INT,
  upid INT,
  thread_name STRING,
  process_name STRING,
  slice_name STRING,
  slice_depth INT,
  height INT,
  leaf_utid INT
) AS
WITH
  span_start AS (
    SELECT
      span.id,
      slice.id AS slice_id,
      MAX(slice.ts, span.ts) AS ts,
      span.ts + span.dur AS span_end_ts,
      slice.ts + slice.dur AS slice_end_ts,
      span.tid,
      span.pid,
      span.utid,
      span.upid,
      span.thread_name,
      span.process_name,
      slice.name AS slice_name,
      slice.depth AS slice_depth,
      span.height,
      span.leaf_utid
    FROM experimental_thread_executing_span_critical_path(NULL, $leaf_utid) span
    JOIN thread_slice slice
      ON
        slice.utid = span.utid
        AND ((slice.ts BETWEEN span.ts AND span.ts + span.dur)
             OR (span.ts BETWEEN slice.ts AND slice.ts + slice.dur))
  )
SELECT
  id,
  slice_id,
  ts,
  MIN(span_end_ts, slice_end_ts) - ts AS dur,
  tid,
  pid,
  utid,
  upid,
  thread_name,
  process_name,
  slice_name,
  slice_depth,
  height,
  leaf_utid
FROM span_start
WHERE MIN(span_end_ts, slice_end_ts) - ts > 0;
