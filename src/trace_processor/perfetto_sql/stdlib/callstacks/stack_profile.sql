--
-- Copyright 2024 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the 'License');
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an 'AS IS' BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- NOTE (psqlnext): the `graphs.hierarchy` and `graphs.scan` modules are DELETED.
-- `_tree_reachable_ancestors_or_self!` (a `graph_reachable_dfs` up-walk) is
-- `TREE WHERE ANCESTOR OF <set> OVER <tree>`; `_graph_aggregating_scan!`
-- SUM-up is `TREE ACCUMULATE UP SUM(self) AS cumulative`.

INCLUDE PERFETTO MODULE v8.jit;

-- The physical callsite tree (one node per callsite) is expanded so that each
-- callsite becomes a chain of its frame's inline frames: one node per symbol in
-- the frame's symbol set, outermost symbol nearest the callsite and innermost at
-- the leaf. The hand-rolled min/max_symbol_id chain arithmetic this replaces is
-- exactly TREE EXPAND DOWN ... ORDERED BY inline_depth (§6.9).
CREATE PERFETTO PIPELINE _callstack_spc_raw_forest MATERIALIZED AS
FROM stack_profile_callsite AS c
|> SELECT id AS callsite_id, parent_id AS parent_callsite_id, frame_id
|> FORK AS callsites
|> TREE EXPAND DOWN (
     FROM callsites
     |> JOIN stack_profile_frame AS f ON callsites.frame_id = f.id
     |> JOIN stack_profile_symbol AS s USING (symbol_set_id)
     |> LEFT JOIN __intrinsic_jit_frame AS jf ON jf.frame_id = f.id
     |> SELECT
          callsites.callsite_id,
          s.id AS symbol_id,
          jf.jit_code_id,
          -- Innermost inline (smallest symbol id) is deepest; it is the leaf.
          row_number() OVER (PARTITION BY callsites.callsite_id ORDER BY s.id DESC)
            AS inline_depth,
          s.id = min(s.id) OVER (PARTITION BY callsites.callsite_id) AS is_leaf
   ) BY callsite_id ORDERED BY inline_depth;

CREATE PERFETTO PIPELINE _callstack_spc_forest MATERIALIZED AS
FROM _callstack_spc_raw_forest AS c
|> JOIN stack_profile_frame AS f ON c.frame_id = f.id
|> LEFT JOIN _v8_js_code AS jsc USING (jit_code_id)
|> LEFT JOIN v8_js_function AS jsf USING (v8_js_function_id)
|> LEFT JOIN _v8_internal_code AS v8c USING (jit_code_id)
|> LEFT JOIN _v8_wasm_code AS wc USING (jit_code_id)
|> LEFT JOIN _v8_regexp_code AS rc USING (jit_code_id)
|> LEFT JOIN __intrinsic_jit_code AS jc ON c.jit_code_id = jc.id
|> LEFT JOIN stack_profile_symbol AS s ON c.symbol_id = s.id
-- The inline-frame tree (id, parent_id) is built by TREE EXPAND DOWN above, so
-- the parent no longer needs reconstructing from a self-join on symbol ids.
|> SELECT
     c.id AS id,
     c.parent_id AS parent_id,
     -- TODO(lalitm): consider demangling in a separate table as
     -- demangling is suprisingly inefficient and is taking a
     -- significant fraction of the runtime on big traces.
     coalesce(
       'JS: ' || iif(jsf.name = "", "(anonymous)", jsf.name) || ':' || jsf.line
       || ':'
       || jsf.col
       || ' ['
       || lower(jsc.tier)
       || ']',
       'WASM: ' || wc.function_name || ' [' || lower(wc.tier) || ']',
       'REGEXP: ' || rc.pattern,
       'V8: ' || v8c.function_name,
       'JIT: ' || jc.function_name,
       demangle(coalesce(s.name, f.deobfuscated_name, f.name)),
       coalesce(s.name, f.deobfuscated_name, f.name, '[Unknown]')
     ) AS name,
     f.mapping AS mapping_id,
     s.source_file,
     coalesce(jsf.line, s.line_number) AS line_number,
     coalesce(jsf.col, 0) AS column_number,
     s.inlined,
     c.callsite_id,
     c.is_leaf AS is_leaf_function_in_callsite_frame
|> ORDER BY id;

-- This index is used to efficiently join the callstack forest with the
-- sample data on callsite_id. This is a key operation in the
-- _callstacks_for_callsites and _callstacks_for_stack_profile_samples macros.
CREATE PERFETTO INDEX _callstack_spc_index ON _callstack_spc_forest(callsite_id);

-- This index is necessary to optimize the leaf-finding query in
-- _callstacks_self_to_cumulative. Without this index, the anti-join on
-- parent_id can be very slow on large traces.
CREATE PERFETTO INDEX _callstack_spc_parent_index ON _callstack_spc_forest(
  parent_id
);

CREATE PERFETTO MACRO _callstacks_for_stack_profile_samples(
  spc_samples TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  -- Keep only the ancestors-or-self of the leaf nodes hit by the samples, over
  -- the full forest, then attach forest + mapping payload.
  FROM _callstack_spc_forest
  |> TREE WHERE ANCESTOR OF (
       SELECT f.id
       FROM $spc_samples s
       JOIN _callstack_spc_forest f USING (callsite_id)
       WHERE f.is_leaf_function_in_callsite_frame
     ) OVER _callstack_spc_forest
  |> JOIN _callstack_spc_forest AS f USING (id)
  |> JOIN stack_profile_mapping AS m ON f.mapping_id = m.id
  |> SELECT
       f.id,
       f.parent_id,
       f.callsite_id,
       f.name,
       m.name AS mapping_name,
       f.source_file,
       f.line_number,
       f.inlined,
       f.is_leaf_function_in_callsite_frame
);

CREATE PERFETTO MACRO _callstacks_for_callsites(samples TableOrSubquery)
RETURNS TableOrSubquery
AS (
  SUBPIPELINE metrics AS (
    FROM $samples
    |> AGGREGATE count() AS self_count GROUP BY callsite_id
  )
  FROM _callstacks_for_stack_profile_samples!(metrics) AS c
  |> LEFT JOIN metrics AS m USING (callsite_id)
  |> SELECT
       c.id,
       c.parent_id,
       c.name,
       c.mapping_name,
       c.source_file,
       c.line_number,
       iif(c.is_leaf_function_in_callsite_frame, coalesce(m.self_count, 0), 0) AS self_count
);

-- Similar to _callstacks_for_callsites but accepts samples with a value column
-- and returns the sum of values instead of count.
-- Input: subquery with (callsite_id, value) columns
-- Output: callstacks with self_value (SUM of value)
CREATE PERFETTO MACRO _callstacks_for_callsites_weighted(
  samples TableOrSubquery
)
RETURNS TableOrSubquery
AS (
  SUBPIPELINE metrics AS (
    FROM $samples
    |> AGGREGATE sum(value) AS self_value GROUP BY callsite_id
  )
  FROM _callstacks_for_stack_profile_samples!(metrics) AS c
  |> LEFT JOIN metrics AS m USING (callsite_id)
  |> SELECT
       c.id,
       c.parent_id,
       c.name,
       c.mapping_name,
       c.source_file,
       c.line_number,
       iif(c.is_leaf_function_in_callsite_frame, coalesce(m.self_value, 0), 0) AS self_value
);

CREATE PERFETTO MACRO _callstacks_self_to_cumulative(callstacks TableOrSubquery)
RETURNS TableOrSubquery
AS (
  -- Cumulative count is the subtree sum of self_count at every node.
  FROM $callstacks
  |> TREE ACCUMULATE UP SUM(self_count) AS cumulative_count
);
