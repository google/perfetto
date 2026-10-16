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

import {STITCHED_CORE_MODULE} from './core_sql';
import type {StitchedSqlModule} from './sql_modules';

// CPU samples hanging below the stitched slices they were taken in, as
// callstacks, weighted by sample count.
//
// Node ids: the stitched nodes keep theirs; a thread's samples outside any
// stitched slice hang below 2^60 + utid; callstack frames below a node get
// 2^61 + a dense rank of (node, frame).
//
// Steps, each materialized by the caller, on top of stitched `nodes`:
//   targets    = _stitched_samples_targets!(nodes, target_ids, end)
//   samples    = _stitched_samples_in!(targets, threads, start, end)
//   attributed = _stitched_samples_attribute!(samples, targets, threads)
//   frames     = _stitched_samples_frames!(attributed)
//   result     = _stitched_samples_nodes!(nodes, attributed, frames)
//
// Only macros: the stdlib modules they use (callstacks.stack_profile and
// intervals.intersect) are included by the steps using them, so that they
// only run when the CPU samples measure is picked.
export const STITCHED_CPU_SAMPLES_MODULE: StitchedSqlModule = {
  name: 'cpu_samples',
  deps: [STITCHED_CORE_MODULE],
  sql: `
-- The slices of the $target_ids nodes (a table with an "id" column) of the
-- stitched $nodes (indexed by "id"), which samples may be attributed to.
CREATE PERFETTO MACRO _stitched_samples_targets(
  nodes TableOrSubquery,
  target_ids TableOrSubquery,
  end_ts Expr
)
RETURNS TableOrSubquery AS (
  SELECT
    n.id,
    s.ts,
    iif(s.dur < 0, $end_ts - s.ts, s.dur) AS dur,
    n.utid,
    s.depth
  FROM $target_ids x
  JOIN $nodes n USING (id)
  JOIN slice s ON s.id = n.slice_id
  WHERE n.utid IS NOT NULL AND iif(s.dur < 0, $end_ts - s.ts, s.dur) > 0
);

-- The samples taken in [$start_ts, $end_ts) on the threads of the $targets
-- or on the $threads (a table with a "utid" column).
CREATE PERFETTO MACRO _stitched_samples_in(
  targets TableOrSubquery,
  threads TableOrSubquery,
  start_ts Expr,
  end_ts Expr
)
RETURNS TableOrSubquery AS (
  WITH sample_threads AS (
    SELECT DISTINCT utid FROM $targets
    UNION
    SELECT utid FROM $threads
  )
  SELECT ss.id, ss.ts, 1 AS dur, tc.utid, ss.callsite_id
  FROM stack_sample ss
  JOIN stack_sample_task_context tc ON tc.id = ss.task_context_id
  JOIN sample_threads st ON st.utid = tc.utid
  WHERE ss.ts >= $start_ts AND ss.ts < $end_ts AND ss.callsite_id IS NOT NULL
);

-- Attributes each of the $samples to the deepest of the $targets running on
-- its thread, or to its thread's "(outside any slice)" node if it is one of
-- the $threads (and drops it otherwise), counting samples per callsite.
CREATE PERFETTO MACRO _stitched_samples_attribute(
  samples TableOrSubquery,
  targets TableOrSubquery,
  threads TableOrSubquery
)
RETURNS TableOrSubquery AS (
  WITH
    sample_ii AS MATERIALIZED (
      SELECT id, ts, dur, utid FROM $samples
    ),
    target_ii AS MATERIALIZED (
      SELECT id, ts, dur, utid FROM $targets
    ),
    hits AS MATERIALIZED (
      SELECT
        ii.id_0 AS sample_id,
        ii.id_1 AS node_id,
        row_number() OVER (PARTITION BY ii.id_0 ORDER BY t.depth DESC) AS rn
      FROM _interval_intersect!((sample_ii, target_ii), (utid)) ii
      JOIN $targets t ON t.id = ii.id_1
    )
  SELECT
    coalesce(h.node_id, 1152921504606846976 + s.utid) AS node_id,
    s.utid,
    s.callsite_id,
    count() AS cnt
  FROM $samples s
  LEFT JOIN hits h ON h.sample_id = s.id AND h.rn = 1
  LEFT JOIN $threads t ON t.utid = s.utid
  WHERE h.node_id IS NOT NULL OR t.utid IS NOT NULL
  GROUP BY 1, 2, 3
);

-- The callstack frames of the $attributed samples, one copy below each node
-- they were attributed to, with the samples' count on the frame of their
-- callsite.
CREATE PERFETTO MACRO _stitched_samples_frames(attributed TableOrSubquery)
RETURNS TableOrSubquery AS (
  WITH
    callsites AS (
      SELECT DISTINCT callsite_id FROM $attributed
    ),
    callsite_frames AS MATERIALIZED (
      SELECT c.callsite_id, f.id AS frame_id
      FROM callsites c
      JOIN _callstack_spc_forest f USING (callsite_id)
      UNION ALL
      SELECT c.callsite_id, f.id AS frame_id
      FROM callsites c
      JOIN experimental_ancestor_stack_profile_callsite(c.callsite_id) a
      JOIN _callstack_spc_forest f ON f.callsite_id = a.id
    ),
    grafted AS MATERIALIZED (
      SELECT
        a.node_id,
        a.utid,
        cf.frame_id,
        f.parent_id AS parent_frame_id,
        f.name,
        sum(iif(
          f.callsite_id = a.callsite_id AND f.is_leaf_function_in_callsite_frame,
          a.cnt,
          0
        )) AS cnt
      FROM $attributed a
      JOIN callsite_frames cf USING (callsite_id)
      JOIN _callstack_spc_forest f ON f.id = cf.frame_id
      GROUP BY a.node_id, a.utid, cf.frame_id
    ),
    ranked AS MATERIALIZED (
      SELECT
        g.*,
        2305843009213693952
          + dense_rank() OVER (ORDER BY g.node_id, g.frame_id) AS id
      FROM grafted g
    )
  SELECT
    r.id,
    coalesce(p.id, r.node_id) AS parent_id,
    r.node_id,
    r.name,
    r.utid,
    r.cnt
  FROM ranked r
  LEFT JOIN ranked p
    ON p.node_id = r.node_id AND p.frame_id = r.parent_frame_id
);

-- The stitched $nodes (without their weights), the "(outside any slice)"
-- thread nodes of the $attributed samples and their callstack $frames, which
-- stand for the slices they were sampled in.
CREATE PERFETTO MACRO _stitched_samples_nodes(
  nodes TableOrSubquery,
  attributed TableOrSubquery,
  frames TableOrSubquery
)
RETURNS TableOrSubquery AS (
  SELECT
    id, parent_id, name, slice_id, slice_ids, utid, upid, is_stitch, kind,
    0 AS self_dur,
    0 AS self_count
  FROM $nodes
  UNION ALL
  SELECT DISTINCT
    node_id AS id,
    NULL AS parent_id,
    '(outside any slice)' AS name,
    NULL AS slice_id,
    NULL AS slice_ids,
    utid,
    NULL AS upid,
    FALSE AS is_stitch,
    'slice' AS kind,
    0 AS self_dur,
    0 AS self_count
  FROM $attributed
  WHERE node_id >= 1152921504606846976
  UNION ALL
  SELECT
    f.id,
    f.parent_id,
    f.name,
    NULL AS slice_id,
    n.slice_ids,
    f.utid,
    NULL AS upid,
    FALSE AS is_stitch,
    'frame' AS kind,
    0 AS self_dur,
    f.cnt AS self_count
  FROM $frames f
  LEFT JOIN $nodes n ON n.id = f.node_id
);
`,
};
