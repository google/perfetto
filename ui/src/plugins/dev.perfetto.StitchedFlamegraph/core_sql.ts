// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import type {StitchedSqlModule} from './sql_modules';

// Shared building blocks of the stitched flamegraph:
//  - `_stitched_parent`: the slice that causally started each slice running
//    elsewhere (flows: binder, message queues, thread pools, coroutines; plus
//    the AIDL slice handling an async binder transaction, from the
//    android.binder module).
//  - `_stitched_track` / `_stitched_thread_label`: thread context of tracks.
//  - `_stitched_finalize!`: turns a tree of stitched nodes into flamegraph
//    rows, inserting a frame where the work hops to another thread.
export const STITCHED_CORE_MODULE: StitchedSqlModule = {
  name: 'core',
  sql: `
INCLUDE PERFETTO MODULE android.binder;
INCLUDE PERFETTO MODULE android.thread;
INCLUDE PERFETTO MODULE graphs.search;

-- For each slice which continues work started by another slice, the slice
-- which started it: the source of its flow, or for the AIDL slice handling an
-- async binder transaction (which follows the 'binder async rcv' slice the
-- transaction flows to), that 'binder async rcv' slice.
CREATE PERFETTO TABLE _stitched_parent(
  id JOINID(slice.id),
  parent_id JOINID(slice.id)
) AS
WITH
  async_aidl AS (
    SELECT s.id, t.binder_reply_id AS parent_id
    FROM android_binder_txns t
    JOIN slice r ON r.id = t.binder_reply_id
    JOIN slice s
      ON s.track_id = r.track_id AND s.ts = t.aidl_ts AND s.name = t.aidl_name
    WHERE NOT t.is_sync AND t.aidl_name IS NOT NULL
  ),
  edges AS (
    SELECT slice_in AS id, slice_out AS parent_id
    FROM flow
    UNION ALL
    SELECT id, parent_id
    FROM async_aidl
  )
SELECT id, min(parent_id) AS parent_id
FROM edges
GROUP BY id
ORDER BY id;

CREATE PERFETTO TABLE _stitched_track(
  track_id JOINID(track.id),
  utid JOINID(thread.id),
  upid JOINID(process.id)
) AS
SELECT id AS track_id, utid, NULL AS upid FROM thread_track
UNION ALL
SELECT id AS track_id, NULL AS utid, upid FROM process_track
ORDER BY track_id;

CREATE PERFETTO INDEX _stitched_track_idx ON _stitched_track(track_id);

CREATE PERFETTO FUNCTION _stitched_basename(path STRING)
RETURNS STRING AS
SELECT replace($path, rtrim($path, replace($path, '/', '')), '');

-- Names of every thread (keyed by utid * 2) and process (keyed by
-- upid * 2 + 1): the thread name as in the timeline (with the instance
-- suffixes of thread pools, e.g. binder:123_4, removed so that the threads
-- of a pool merge), and the "thread (process)" label naming it in full.
CREATE PERFETTO TABLE _stitched_thread_label(
  thread_key LONG,
  upid JOINID(process.id),
  thread_name STRING,
  label STRING
) AS
WITH threads AS (
  SELECT
    t.utid,
    t.upid,
    coalesce(android_standardize_thread_name(t.name), 'tid ' || t.tid)
      AS thread_name,
    _stitched_basename(p.name) AS process_name
  FROM thread t
  LEFT JOIN process p USING (upid)
)
SELECT
  utid * 2 AS thread_key,
  upid,
  thread_name,
  thread_name || coalesce(' (' || process_name || ')', '') AS label
FROM threads
UNION ALL
SELECT
  upid * 2 + 1 AS thread_key,
  upid,
  coalesce(_stitched_basename(name), 'pid ' || pid) AS thread_name,
  coalesce(_stitched_basename(name), 'pid ' || pid) AS label
FROM process
ORDER BY thread_key;

CREATE PERFETTO INDEX _stitched_thread_label_idx
ON _stitched_thread_label(thread_key);

-- Turns a table of stitched nodes into flamegraph rows.
--
-- $nodes must be a table indexed by "id" with the columns:
--   id LONG, parent_id LONG: the tree (parent_id NULL for roots).
--   name STRING: the node name.
--   slice_id LONG: the slice the node stands for (NULL if none).
--   slice_ids STRING: comma separated ids of all the slices the node stands
--     for, when several were merged into it (else the slice_id).
--   utid LONG, upid LONG: the thread (or process) the node ran on.
--   is_stitch BOOL: whether the node continues work of its parent on another
--     work item; always true when the parent ran on another thread.
--   kind STRING: 'slice', or 'frame' for nodes which never start a run of
--     work on a thread (e.g. callstack frames below a slice).
--   self_dur LONG, self_count LONG: the node's own weights.
--
-- A frame, carrying no weight, is inserted above every node that starts a
-- run of work: roots and nodes reached through a stitch. It stands for the
-- slices of the node, is named after where the work runs and its kind says
-- how it got there:
--   'thread'       roots: "thread (process)".
--   'process_hop'  from another process (e.g. binder): "thread (process)".
--   'hop'          from another thread of the same process: "thread".
--   'requeue'      from the same thread (e.g. a message posted to its own
--                  queue): "same thread".
CREATE PERFETTO MACRO _stitched_finalize(nodes TableOrSubquery)
RETURNS TableOrSubquery AS (
  WITH framed AS (
    SELECT
      n.id,
      n.parent_id,
      n.slice_ids,
      l.label,
      l.thread_name,
      CASE
        WHEN n.parent_id IS NULL THEN 'thread'
        WHEN pl.thread_key = l.thread_key THEN 'requeue'
        WHEN pl.upid = l.upid THEN 'hop'
        ELSE 'process_hop'
      END AS kind
    FROM $nodes n
    LEFT JOIN $nodes pn ON pn.id = n.parent_id
    LEFT JOIN _stitched_thread_label l
      ON l.thread_key = coalesce(n.utid * 2, n.upid * 2 + 1)
    LEFT JOIN _stitched_thread_label pl
      ON pl.thread_key = coalesce(pn.utid * 2, pn.upid * 2 + 1)
    WHERE n.kind != 'frame' AND (n.parent_id IS NULL OR n.is_stitch)
  )
  SELECT
    id * 2 + 1 AS id,
    parent_id * 2 AS parentId,
    CASE kind
      WHEN 'requeue' THEN 'same thread'
      WHEN 'hop' THEN coalesce(thread_name, '[unknown thread]')
      ELSE coalesce(label, '[unknown thread]')
    END AS name,
    kind,
    coalesce(label, '') AS thread,
    slice_ids,
    0 AS self_dur,
    0 AS self_count
  FROM framed
  UNION ALL
  SELECT
    n.id * 2 AS id,
    iif(
      n.kind != 'frame' AND (n.parent_id IS NULL OR n.is_stitch),
      n.id * 2 + 1,
      n.parent_id * 2
    ) AS parentId,
    coalesce(nullif(n.name, ''), '[unnamed]') AS name,
    n.kind,
    coalesce(l.label, '') AS thread,
    n.slice_ids,
    n.self_dur,
    n.self_count
  FROM $nodes n
  LEFT JOIN _stitched_thread_label l
    ON l.thread_key = coalesce(n.utid * 2, n.upid * 2 + 1)
);
`,
};
