-- =============================================================================
-- Representative PerfettoSQL Language Examples
-- =============================================================================
-- This file contains representative examples of PerfettoSQL syntax, gathered
-- from the Perfetto codebase stdlib. It covers the major language constructs.
-- =============================================================================


-- =============================================================================
-- 1. INCLUDE PERFETTO MODULE
-- =============================================================================
-- Imports SQL modules for reuse. Modules contain tables, views, functions, macros.

INCLUDE PERFETTO MODULE android.frames.timeline;
INCLUDE PERFETTO MODULE intervals.intersect;
INCLUDE PERFETTO MODULE slices.with_context;
INCLUDE PERFETTO MODULE wattson.cpu.idle;
INCLUDE PERFETTO MODULE wattson.device_infos;
INCLUDE PERFETTO MODULE wattson.estimates;
INCLUDE PERFETTO MODULE graphs.hierarchy;
INCLUDE PERFETTO MODULE graphs.scan;


-- =============================================================================
-- 2. SELECT STATEMENTS
-- =============================================================================

-- Basic SELECT with JOINs and string functions (from stdlib/android/input.sql)
SELECT
  str_split(str_split(slice.name, '=', 3), ')', 0) AS event_type,
  str_split(str_split(slice.name, '=', 2), ',', 0) AS event_seq,
  str_split(str_split(slice.name, '=', 1), ',', 0) AS event_channel,
  thread.tid,
  thread.name AS thread_name,
  process.upid,
  process.pid,
  process.name AS process_name,
  slice.ts,
  slice.dur,
  slice.track_id
FROM slice
JOIN thread_track ON thread_track.id = slice.track_id
JOIN thread USING (utid)
JOIN process USING (upid)
WHERE slice.name GLOB 'sendMessage(*'
ORDER BY event_seq;

-- SELECT with window functions: LAG, LEAD (from stdlib/sched/thread_executing_span.sql)
SELECT
  utid,
  id,
  waker_id,
  ts,
  idle_state,
  idle_reason,
  ts - idle_ts AS idle_dur,
  is_idle_reason_self,
  lag(id) OVER (PARTITION BY utid ORDER BY ts) AS prev_id,
  lead(id) OVER (PARTITION BY utid ORDER BY ts) AS next_id,
  coalesce(lead(idle_ts) OVER (PARTITION BY utid ORDER BY ts), thread_end_ts) - ts AS dur,
  lead(is_idle_reason_self) OVER (PARTITION BY utid ORDER BY ts) AS is_next_idle_reason_self
FROM _wakeup_events
ORDER BY id;

-- SELECT with CASE, aggregation, GROUP BY (from stdlib/android/memory/heap_graph/class_tree.sql)
SELECT
  coalesce(c.deobfuscated_name, c.name) AS class_name,
  o.heap_type,
  o.root_type,
  o.reachable,
  sum(o.self_size) AS total_size,
  sum(o.native_size) AS total_native_size,
  count() AS count
FROM heap_graph_object AS o
JOIN heap_graph_class AS c ON o.type_id = c.id
GROUP BY class_name, o.heap_type, o.root_type, o.reachable;

-- SELECT with CTEs, extract_arg, and counter tracks (from stdlib/prelude/after_eof/counters.sql)
SELECT
  id,
  name,
  NULL AS parent_id,
  type,
  dimension_arg_set_id,
  source_arg_set_id,
  machine_id,
  counter_unit AS unit,
  extract_arg(source_arg_set_id, 'description') AS description
FROM __intrinsic_track
WHERE event_type = 'counter';

-- SELECT with rate calculation using LEAD window function (from stdlib/wattson/cpu/arm_dsu.sql)
SELECT
  ts,
  lead(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts AS dur,
  value / (
    lead(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts
  ) AS access_rate
FROM counter AS c
JOIN counter_track AS t
  ON c.track_id = t.id
WHERE
  t.name = 'arm_l3_cache_miss';


-- =============================================================================
-- 3. CREATE PERFETTO TABLE
-- =============================================================================

-- Table with typed columns, CTEs, and string manipulation (from stdlib/android/device.sql)
CREATE PERFETTO TABLE android_device_name(
  -- Device name.
  name STRING,
  -- Machine identifier
  machine_id JOINID(machine.id)
) AS
WITH
  after_first_slash(str, machine_id) AS (
    SELECT
      substr(android_build_fingerprint, instr(android_build_fingerprint, '/') + 1) AS str,
      id AS machine_id
    FROM machine
    WHERE android_build_fingerprint IS NOT NULL
  ),
  after_second_slash(str, machine_id) AS (
    SELECT
      substr(str, instr(str, '/') + 1) AS str,
      machine_id
    FROM after_first_slash
  ),
  before_colon(str, machine_id) AS (
    SELECT
      substr(str, 0, instr(str, ':')) AS str,
      machine_id
    FROM after_second_slash
  )
SELECT
  str AS name,
  machine_id
FROM before_colon;

-- Table with UNION ALL (from stdlib/android/input.sql)
CREATE PERFETTO TABLE _input_event_frame_association AS
SELECT * FROM _input_event_frame_intersections
UNION ALL
SELECT * FROM _input_event_frame_speculative_matches;

-- Table with window functions and complex joins (from stdlib/sched/thread_executing_span.sql)
CREATE PERFETTO TABLE _wakeup_graph AS
WITH
  _wakeup_events AS (
    SELECT
      utid,
      thread_end_ts,
      iif(is_irq, 'IRQ', state) AS idle_state,
      blocked_function AS idle_reason,
      _wakeup.id,
      iif(is_irq, NULL, _wakeup_map.id) AS waker_id,
      _wakeup.ts,
      prev_end_ts AS idle_ts,
      iif(
        is_irq OR _wakeup_map.id IS NULL OR (NOT state IS NULL AND state != 'S'),
        1, 0
      ) AS is_idle_reason_self
    FROM _wakeup
    LEFT JOIN _wakeup_map USING (waker_id)
  )
SELECT
  utid,
  id,
  waker_id,
  ts,
  idle_state,
  idle_reason,
  ts - idle_ts AS idle_dur,
  is_idle_reason_self,
  lag(id) OVER (PARTITION BY utid ORDER BY ts) AS prev_id,
  lead(id) OVER (PARTITION BY utid ORDER BY ts) AS next_id,
  coalesce(lead(idle_ts) OVER (PARTITION BY utid ORDER BY ts), thread_end_ts) - ts AS dur,
  lead(is_idle_reason_self) OVER (PARTITION BY utid ORDER BY ts) AS is_next_idle_reason_self
FROM _wakeup_events
ORDER BY id;


-- =============================================================================
-- 4. CREATE PERFETTO VIEW
-- =============================================================================

-- View with typed columns and intrinsic table (from stdlib/prelude/after_eof/counters.sql)
CREATE PERFETTO VIEW counter_track(
  -- Unique identifier for this cpu counter track.
  id ID(track.id),
  -- Name of the track.
  name STRING,
  -- The track which is the "parent" of this track.
  parent_id JOINID(track.id),
  -- The type of a track indicates the type of data the track contains.
  type STRING,
  -- The dimensions of the track which uniquely identify the track.
  dimension_arg_set_id ARGSETID,
  -- Args for this track which store information about "source" of this track.
  source_arg_set_id ARGSETID,
  -- Machine identifier
  machine_id JOINID(machine.id),
  -- The units of the counter.
  unit STRING,
  -- The description for this track.
  description STRING
) AS
SELECT
  id,
  name,
  NULL AS parent_id,
  type,
  dimension_arg_set_id,
  source_arg_set_id,
  machine_id,
  counter_unit AS unit,
  extract_arg(source_arg_set_id, 'description') AS description
FROM __intrinsic_track
WHERE event_type = 'counter';

-- View with hash-based stack IDs and subqueries (from stdlib/slices/stack.sql)
CREATE PERFETTO VIEW slice_with_stack_id(
  -- Slice id.
  id ID(slice.id),
  -- Alias of `slice.ts`.
  ts TIMESTAMP,
  -- Alias of `slice.dur`.
  dur DURATION,
  -- Alias of `slice.track_id`.
  track_id JOINID(track.id),
  -- Alias of `slice.category`.
  category STRING,
  -- Alias of `slice.name`.
  name STRING,
  -- A unique identifier obtained from the names and categories of all slices
  stack_id LONG,
  -- The stack_id for the parent of this slice.
  parent_stack_id LONG
) AS
WITH
  slice_stack_hashes AS (
    SELECT
      s.id,
      coalesce(
        (
          SELECT
            hash(GROUP_CONCAT(hash(coalesce(category, '') || '|' || name), '|'))
          FROM _slice_ancestor_and_self(s.id)
          ORDER BY depth ASC
        ),
        0
      ) AS stack_hash
    FROM slice AS s
  )
SELECT
  s.id,
  s.ts,
  s.dur,
  s.track_id,
  s.category,
  s.name,
  sh.stack_hash AS stack_id,
  coalesce(parent_sh.stack_hash, 0) AS parent_stack_id
FROM slice AS s
JOIN slice_stack_hashes AS sh ON s.id = sh.id
LEFT JOIN slice_stack_hashes AS parent_sh ON s.parent_id = parent_sh.id;

-- View with UNION ALL combining different track types (from stdlib/viz/slices.sql)
CREATE PERFETTO VIEW _viz_slices_for_ui_table AS
SELECT * FROM thread_or_process_slice
UNION ALL
SELECT
  slice.id,
  slice.ts,
  slice.dur,
  slice.category,
  slice.name,
  slice.track_id,
  track.name AS track_name,
  NULL AS thread_name,
  NULL AS utid,
  NULL AS tid,
  NULL AS process_name,
  NULL AS upid,
  NULL AS pid,
  slice.depth,
  slice.parent_id,
  slice.arg_set_id
FROM slice
JOIN track ON slice.track_id = track.id
WHERE NOT (slice.track_id IN (SELECT id FROM process_track))
  AND NOT (slice.track_id IN (SELECT id FROM thread_track));


-- =============================================================================
-- 5. CREATE PERFETTO FUNCTION
-- =============================================================================

-- Function returning a TABLE with rate calculation (from stdlib/wattson/cpu/arm_dsu.sql)
CREATE PERFETTO FUNCTION _get_rate(
  -- Name of the counter event.
  event STRING
)
RETURNS TABLE(
  ts TIMESTAMP,
  dur DURATION,
  access_rate LONG
) AS
SELECT
  ts,
  lead(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts AS dur,
  value / (
    lead(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts
  ) AS access_rate
FROM counter AS c
JOIN counter_track AS t
  ON c.track_id = t.id
WHERE
  t.name = $event;

-- Function returning a TABLE with critical path computation
-- (from stdlib/sched/thread_executing_span.sql)
CREATE PERFETTO FUNCTION _thread_executing_span_critical_path(
  -- Utid of the thread to compute the critical path for.
  root_utid JOINID(thread.id),
  -- Timestamp.
  ts TIMESTAMP,
  -- Duration.
  dur DURATION
)
RETURNS TABLE(
  -- Thread Utid the critical path was filtered to.
  root_utid JOINID(thread.id),
  -- Id of thread executing span.
  root_id LONG,
  -- Id of the first (runnable) thread state.
  id LONG,
  -- Timestamp of first thread_state.
  ts TIMESTAMP,
  -- Duration of thread_executing_span.
  dur DURATION,
  -- Utid of thread with thread_state.
  utid JOINID(thread.id)
) AS
SELECT
  root_utid,
  root_id,
  id,
  ts,
  dur,
  utid
FROM _critical_path_by_intervals!(
  (SELECT $root_utid AS utid, $ts AS ts, $dur AS dur),
  _wakeup_graph
);

-- Scalar function that delegates to a C++ intrinsic (from stdlib/std/trees/filter.sql)
CREATE PERFETTO FUNCTION _tree_constraint(
  -- Column name to filter on
  column STRING,
  -- Operator: '=', '!=', '<', '>', '<=', '>=', 'GLOB', etc.
  op STRING,
  -- Value to compare against (can be any type)
  value ANY
)
-- Returns a constraint pointer
RETURNS ANY
DELEGATES TO __intrinsic_tree_constraint;


-- =============================================================================
-- 6. CREATE PERFETTO MACRO
-- =============================================================================
-- Macros are template-based query generators. They accept TableOrSubquery,
-- ColumnName, and Expr parameters. Invoked with ! syntax: macro_name!(args).

-- Macro accepting a TableOrSubquery and returning a TableOrSubquery
-- (from stdlib/callstacks/stack_profile.sql)
CREATE PERFETTO MACRO _callstacks_for_stack_profile_samples(
  -- The source samples table.
  spc_samples TableOrSubquery
)
RETURNS TableOrSubquery AS
(
  SELECT
    f.id,
    f.parent_id,
    f.callsite_id,
    f.name,
    m.name AS mapping_name,
    f.source_file,
    f.line_number,
    f.inlined,
    f.is_leaf_function_in_callsite_frame
  FROM _tree_reachable_ancestors_or_self!(
    _callstack_spc_forest,
    (
      SELECT f.id
      FROM $spc_samples s
      JOIN _callstack_spc_forest f USING (callsite_id)
      WHERE f.is_leaf_function_in_callsite_frame
    )
  ) AS g
  JOIN _callstack_spc_forest AS f
    USING (id)
  JOIN stack_profile_mapping AS m
    ON f.mapping_id = m.id
);

-- Macro with MATERIALIZED CTE and weighted aggregation
-- (from stdlib/callstacks/stack_profile.sql)
CREATE PERFETTO MACRO _callstacks_for_callsites_weighted(
  -- The weighted samples table.
  samples TableOrSubquery
)
RETURNS TableOrSubquery AS
(
  WITH
    metrics AS MATERIALIZED (
      SELECT
        callsite_id,
        sum(value) AS self_value
      FROM $samples
      GROUP BY callsite_id
    )
  SELECT
    c.id,
    c.parent_id,
    c.name,
    c.mapping_name,
    c.source_file,
    c.line_number,
    iif(c.is_leaf_function_in_callsite_frame, coalesce(m.self_value, 0), 0) AS self_value
  FROM _callstacks_for_stack_profile_samples!(metrics) AS c
  LEFT JOIN metrics AS m
    USING (callsite_id)
);

-- Macro with multiple parameters including ColumnName and Expr types,
-- plus a recursive CTE (from stdlib/intervals/mipmap.sql)
CREATE PERFETTO MACRO _mipmap_buckets_table(
  -- The source table containing ts, dur, and the partition column.
  _source_table TableOrSubQuery,
  -- The column name to partition the data by.
  _partition_column ColumnName,
  -- The duration of each bucket in nanoseconds.
  _bucket_duration_ns Expr
)
RETURNS TableOrSubQuery AS
(
  WITH
  RECURSIVE bucket_meta AS (
      SELECT
        $_partition_column,
        trace_min_ts,
        trace_max_ts,
        bucket_duration_ns,
        bucket_count
      FROM _mipmap_bucket_metadata!($_source_table, $_partition_column, $_bucket_duration_ns)
    ),
    buckets_rec($_partition_column, bucket_index, ts, dur) AS (
      SELECT
        $_partition_column,
        0 AS bucket_index,
        trace_min_ts,
        bucket_duration_ns
      FROM bucket_meta
      UNION ALL
      SELECT
        $_partition_column,
        bucket_index + 1,
        ts + dur,
        bucket_duration_ns
      FROM buckets_rec
      WHERE bucket_index + 1 < (SELECT bucket_count FROM bucket_meta LIMIT 1)
    )
  SELECT
    row_number() OVER (ORDER BY $_partition_column, bucket_index) AS id,
    $_partition_column,
    bucket_index,
    ts,
    dur
  FROM buckets_rec
);

-- Macro with _graph_aggregating_scan (from stdlib/viz/slices.sql)
CREATE PERFETTO MACRO _viz_slice_ancestor_agg(
  inits TableOrSubquery,
  nodes TableOrSubquery
)
RETURNS TableOrSubquery
AS
(
  SELECT
    id,
    parent_id AS parentId,
    name,
    self_dur,
    self_count,
    1 AS simple_count
  FROM _graph_aggregating_scan!(
    (
      SELECT id AS source_node_id, parent_id AS dest_node_id
      FROM $nodes
      WHERE parent_id IS NOT NULL
    ),
    (SELECT id, dur, dur AS self_dur, 1 AS self_count FROM $inits),
    (dur, self_dur, self_count),
    (
      WITH agg AS (
        SELECT t.id, sum(t.dur) AS child_dur
        FROM $table t
        GROUP BY id
      )
      SELECT a.id, s.dur, s.dur - a.child_dur AS self_dur, 0 AS self_count
      FROM agg a
      JOIN $nodes s USING (id)
    )
  ) g
  JOIN $nodes s USING (id)
);


-- =============================================================================
-- 7. CREATE PERFETTO INDEX
-- =============================================================================

CREATE PERFETTO INDEX _input_consumers_lookup_idx
  ON _input_consumers_lookup(cookie);

CREATE PERFETTO INDEX _callstack_spc_index
  ON _callstack_spc_forest(callsite_id);

CREATE PERFETTO INDEX _callstack_spc_parent_index
  ON _callstack_spc_forest(parent_id);


-- =============================================================================
-- 8. VIRTUAL TABLES (Perfetto-specific span join operators)
-- =============================================================================

CREATE VIRTUAL TABLE _arm_l3_rates USING SPAN_OUTER_JOIN(
  _arm_l3_miss_rate,
  _arm_l3_hit_rate
);


-- =============================================================================
-- 9. NOTABLE PERFETTO-SPECIFIC FEATURES
-- =============================================================================

-- Type annotations in column definitions:
--   STRING, LONG, BOOL, TIMESTAMP, DURATION
--   ID(table.column)       -- primary key reference
--   JOINID(table.column)   -- foreign key reference
--   ARGSETID               -- argument set reference
--   ANY                    -- any type

-- Macro invocation with ! syntax:
--   _interval_intersect!(args)
--   _tree_reachable_ancestors_or_self!(tree_table, start_nodes)
--   _critical_path_by_intervals!(intervals, graph)

-- Parameter references with $ prefix:
--   $event, $slice_id, $root_utid, $ts, $dur
--   $_partition_column (ColumnName params in macros)
--   $samples, $window_table (TableOrSubquery params in macros)

-- Perfetto-specific built-in functions:
--   extract_arg(arg_set_id, 'key')  -- extract from arg sets
--   str_split(str, delimiter, idx)  -- split strings
--   hash(value)                     -- hash function
--   iif(cond, true_val, false_val)  -- inline if

-- DELEGATES TO for C++ intrinsic function binding:
--   RETURNS ANY DELEGATES TO __intrinsic_tree_constraint;

-- GLOB pattern matching (SQLite extension):
--   WHERE slice.name GLOB 'sendMessage(*'
