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

-- These are internal views for V8 CPU profiler data. The legacy DevTools JSON
-- exporter is the only supported consumer.
--
-- V8 CPU samples flow through the generic Perfetto profiling tables
-- (`stack_profile_frame`, `stack_profile_callsite`, `cpu_profile_stack_sample`).
-- V8-only attributes — tier, deopt reason, leaf line/column, sample kind,
-- session bounds — live in the `__intrinsic_v8_*` sidecar tables.

-- Per-frame V8 attributes joined onto a `stack_profile_frame` row.
CREATE PERFETTO VIEW _v8_cpu_profile_frame(
  -- Frame this row attaches to.
  frame_id JOINID(stack_profile_frame.id),
  -- Function name.
  function_name STRING,
  -- Source URL.
  url STRING,
  -- Source line for the frame.
  line_number LONG,
  -- Source column for the frame.
  column_number LONG,
  -- Compilation tier.
  tier STRING,
  -- Whether the frame represents an inlined call site.
  is_inlined LONG,
  -- Deopt reason.
  deopt_reason STRING,
  -- V8 script id for DevTools source mapping.
  script_id LONG
)
AS
SELECT
  spf.id AS frame_id,
  spf.name AS function_name,
  sym.source_file AS url,
  sym.line_number AS line_number,
  vspf.column_number AS column_number,
  vspf.tier AS tier,
  vspf.is_inlined AS is_inlined,
  vspf.deopt_reason AS deopt_reason,
  vspf.script_id AS script_id
FROM stack_profile_frame AS spf
-- StackProfileSequenceState invokes V8CpuProfileModule::OnFrameInterned every
-- time a (sequence-local) Frame iid resolves to a stack_profile_frame row,
-- which is once per V8 frame_iid emitted on each sequence. Multiple V8
-- frame_iids can collapse to the same frame_id (deduped by InternFrame on
-- function/source/line), and the same frame_iid may be re-emitted after a
-- SEQ_INCREMENTAL_STATE_CLEARED, so __intrinsic_v8_stack_profile_frame can
-- contain multiple identical rows per frame_id. Group to one representative
-- row per frame_id so downstream joins do not multiply.
LEFT JOIN (
  SELECT
    frame_id,
    min(column_number) AS column_number,
    min(tier) AS tier,
    min(is_inlined) AS is_inlined,
    min(deopt_reason) AS deopt_reason,
    min(script_id) AS script_id
  FROM __intrinsic_v8_stack_profile_frame
  GROUP BY
    frame_id
) AS vspf
  ON vspf.frame_id = spf.id
-- The legacy C++ exporter takes the first matching symbol for a frame's
-- symbol_set_id.
LEFT JOIN (
  SELECT
    symbol_set_id,
    source_file,
    line_number,
    row_number() OVER (PARTITION BY symbol_set_id ORDER BY id) AS _rn
  FROM stack_profile_symbol
) AS sym
  ON sym.symbol_set_id = spf.symbol_set_id
  AND sym._rn = 1;

-- Per-sample V8 attributes joined onto a `cpu_profile_stack_sample` row.
CREATE PERFETTO VIEW _v8_cpu_profile_sample(
  -- Sample this row attaches to.
  cpu_profile_stack_sample_id JOINID(cpu_profile_stack_sample.id),
  -- Sample timestamp in nanoseconds.
  ts TIMESTAMP,
  -- Thread the sample was taken on.
  utid JOINID(thread.utid),
  -- Leaf callsite hit by the sample.
  callsite_id JOINID(stack_profile_callsite.id),
  -- Sample origin.
  sample_kind STRING,
  -- Per-sample leaf source line.
  leaf_line LONG,
  -- Per-sample leaf source column.
  leaf_column LONG,
  -- V8 CPU profile session owning the sample.
  session_id LONG
)
AS
SELECT
  cps.id AS cpu_profile_stack_sample_id,
  cps.ts AS ts,
  cps.utid AS utid,
  cps.callsite_id AS callsite_id,
  vcs.sample_kind AS sample_kind,
  vcs.leaf_line AS leaf_line,
  vcs.leaf_column AS leaf_column,
  vcs.session_id AS session_id
FROM cpu_profile_stack_sample AS cps
LEFT JOIN __intrinsic_v8_cpu_profile_sample AS vcs
  ON vcs.cpu_profile_stack_sample_id = cps.id;

-- Session-scoped metadata for a V8 CPU profile.
CREATE PERFETTO VIEW _v8_cpu_profile_session(
  -- Internal trace_processor session row id.
  v8_cpu_profile_session_id LONG,
  -- V8-internal session id (stable across START/END).
  session_id LONG,
  -- Thread the profiler ran on.
  utid JOINID(thread.utid),
  -- Source string.
  source STRING,
  -- TraceProcessor timestamp (ns) of the session start.
  start_ts TIMESTAMP,
  -- TraceProcessor timestamp (ns) of the session end.
  end_ts TIMESTAMP,
  -- V8 wall-clock startTime in microseconds.
  start_time_us LONG,
  -- V8 wall-clock endTime in microseconds.
  end_time_us LONG,
  -- Thread timestamp at start (ns).
  start_thread_ts TIMESTAMP,
  -- Thread timestamp at end (ns).
  end_thread_ts TIMESTAMP
)
AS
SELECT
  id AS v8_cpu_profile_session_id,
  session_id,
  utid,
  source,
  start_ts,
  end_ts,
  start_time_us,
  end_time_us,
  start_thread_ts,
  end_thread_ts
FROM __intrinsic_v8_cpu_profile_session;

-- ---------------------------------------------------------------------------
-- Internal views shaped for the legacy DevTools JSON exporter.
--
-- These are not stable API; they exist so the C++ JSON exporter can be a thin
-- row-iteration loop instead of hand-rolling joins and per-session
-- bookkeeping. They take the canonical sidecar-joined views above and bake in
-- the legacy export's two pieces of per-session state:
--   * a monotonic integer node id derived from `(session, callsite_id)`.
--   * per-sample wall-clock deltas in microseconds, relative to the previous
--     sample.
-- ---------------------------------------------------------------------------
CREATE PERFETTO VIEW _v8_cpu_profile_legacy_export_node AS
WITH RECURSIVE
  -- Distinct leaf callsites seen by samples in each session.
  _session_leaves AS (
    SELECT DISTINCT s.v8_cpu_profile_session_id, vs.callsite_id
    FROM _v8_cpu_profile_session AS s
    JOIN _v8_cpu_profile_sample AS vs
      ON vs.session_id = s.session_id
      AND vs.utid = s.utid
      AND vs.ts >= s.start_ts
      AND vs.ts <= coalesce(s.end_ts, 9223372036854775807)
  ),
  -- Closure: leaves plus all ancestors via parent_id.
  _session_callsites(v8_cpu_profile_session_id, callsite_id) AS (
    SELECT v8_cpu_profile_session_id, callsite_id FROM _session_leaves
    UNION
    SELECT sc.v8_cpu_profile_session_id, spc.parent_id
    FROM _session_callsites AS sc
    JOIN stack_profile_callsite AS spc
      ON spc.id = sc.callsite_id
    WHERE
      spc.parent_id IS NOT NULL
  ),
  _session_nodes AS (
    SELECT
      sc.v8_cpu_profile_session_id,
      sc.callsite_id,
      spc.parent_id AS parent_callsite_id,
      spc.frame_id,
      row_number() OVER (
        PARTITION BY
          sc.v8_cpu_profile_session_id
        ORDER BY sc.callsite_id
      ) AS node_id
    FROM _session_callsites AS sc
    JOIN stack_profile_callsite AS spc
      ON spc.id = sc.callsite_id
  )
SELECT
  sn.v8_cpu_profile_session_id,
  sn.callsite_id,
  sn.node_id,
  parent.node_id AS parent_node_id,
  coalesce(vfr.function_name, '') AS function_name,
  vfr.url AS url,
  vfr.line_number AS line_number,
  vfr.column_number AS column_number,
  CASE vfr.tier
    WHEN 'IGNITION' THEN 'JS'
    WHEN 'SPARKPLUG' THEN 'JS'
    WHEN 'MAGLEV' THEN 'JS'
    WHEN 'TURBOFAN' THEN 'JS'
    WHEN 'WASM' THEN 'WASM'
    WHEN 'REGEXP' THEN 'REGEXP'
    ELSE 'other'
  END AS code_type,
  vfr.deopt_reason AS deopt_reason,
  vfr.script_id AS script_id
FROM _session_nodes AS sn
LEFT JOIN _v8_cpu_profile_frame AS vfr
  ON vfr.frame_id = sn.frame_id
LEFT JOIN _session_nodes AS parent
  ON parent.v8_cpu_profile_session_id = sn.v8_cpu_profile_session_id
  AND parent.callsite_id = sn.parent_callsite_id
ORDER BY
  sn.v8_cpu_profile_session_id,
  sn.node_id;

CREATE PERFETTO VIEW _v8_cpu_profile_legacy_export_sample AS
SELECT
  s.v8_cpu_profile_session_id,
  sn.node_id,
  CAST((vs.ts
  - lag(vs.ts, 1, s.start_ts) OVER (
    PARTITION BY
      s.v8_cpu_profile_session_id
    ORDER BY vs.ts
  ))
  / 1000 AS LONG) AS delta_us,
  coalesce(vs.leaf_line, 0) AS leaf_line,
  coalesce(vs.leaf_column, 0) AS leaf_column,
  vs.sample_kind AS sample_kind
FROM _v8_cpu_profile_session AS s
JOIN _v8_cpu_profile_sample AS vs
  ON vs.session_id = s.session_id
  AND vs.utid = s.utid
  AND vs.ts >= s.start_ts
  AND vs.ts <= coalesce(s.end_ts, 9223372036854775807)
JOIN _v8_cpu_profile_legacy_export_node AS sn
  ON sn.v8_cpu_profile_session_id = s.v8_cpu_profile_session_id
  AND sn.callsite_id = vs.callsite_id
ORDER BY
  s.v8_cpu_profile_session_id,
  vs.ts;

-- Per-session rows the exporter iterates for the surrounding `Profile` /
-- closing `ProfileChunk` events.
CREATE PERFETTO VIEW _v8_cpu_profile_legacy_export_session AS
SELECT
  v8_cpu_profile_session_id,
  session_id,
  utid,
  start_ts,
  end_ts,
  start_time_us,
  end_time_us,
  start_thread_ts,
  end_thread_ts,
  source
FROM _v8_cpu_profile_session
ORDER BY
  start_ts;
