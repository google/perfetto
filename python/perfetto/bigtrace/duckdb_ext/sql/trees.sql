-- Copyright (C) 2026 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--      http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- Tree operators (docs/trees.md). Native kernels: src/trees.cc.
--
-- Every operator takes relations as table/view names (or a parenthesised
-- query string) with columns id, parent_id (NULL = root), and `per` partition
-- columns (e.g. ['_trace_uuid']); each partition is an independent forest.
-- Aggregates are SQL strings 'expr AS name'. Internal names start with __pf_.
-- Kernels are aggregates (GROUP BY per) returning LIST<STRUCT(id, ...)>.

-- Helpers ---------------------------------------------------------------------
CREATE OR REPLACE MACRO pf_tree__src(r) AS
  CASE
    WHEN regexp_matches(trim(r), '(?is)\|>') OR regexp_matches(trim(r), '(?is)^\s*\(?\s*(from|interval|intervals|graph)\b')
    THEN '(' || regexp_replace(trim(pf_compile_pipe(r)), ';\s*$', '') || ')'
    WHEN regexp_matches(trim(r), '(?is)^(select|with|values)\b|^\(')
    THEN '(' || regexp_replace(trim(r), ';\s*$', '') || ')'
    ELSE regexp_replace(trim(r), ';\s*$', '')
  END
-- @@
CREATE OR REPLACE MACRO pf_tree__to_list(x) AS
  CASE
    WHEN x IS NULL THEN []::VARCHAR[]
    WHEN json_type(to_json(x)) = 'ARRAY' THEN from_json(to_json(x), '["VARCHAR"]')
    WHEN trim(x::VARCHAR) = '' THEN []::VARCHAR[]
    ELSE [trim(x::VARCHAR)]
  END
-- @@
CREATE OR REPLACE MACRO pf_tree__k(per) AS
  array_to_string(list_transform(pf_tree__to_list(per), c -> c || ', '), '')
-- @@
CREATE OR REPLACE MACRO pf_tree__grp(per) AS
  CASE WHEN len(pf_tree__to_list(per)) = 0 THEN '' ELSE ' GROUP BY ' || array_to_string(pf_tree__to_list(per), ', ') END
-- @@
CREATE OR REPLACE MACRO pf_tree__on(per, a, b) AS
  array_to_string(list_transform(pf_tree__to_list(per), c -> a || '.' || c || ' = ' || b || '.' || c || ' AND '), '')
-- @@
CREATE OR REPLACE MACRO pf_tree__using(per) AS
  array_to_string(list_append(pf_tree__to_list(per), 'id'), ', ')
-- @@
CREATE OR REPLACE MACRO pf_tree__strlist(xs) AS
  'list_value(' || array_to_string(list_transform(pf_tree__to_list(xs), x -> '''' || x || ''''), ', ') || ')::VARCHAR[]'
-- @@
CREATE OR REPLACE MACRO pf_tree__agg_expr(s) AS
  regexp_extract(s, '^(.*\S)\s+(?i:as)\s+(\w+)\s*$', 1)
-- @@
CREATE OR REPLACE MACRO pf_tree__agg_name(s) AS
  CASE WHEN regexp_matches(s, '^(.*\S)\s+(?i:as)\s+(\w+)\s*$')
       THEN regexp_extract(s, '^(.*\S)\s+(?i:as)\s+(\w+)\s*$', 2)
       ELSE error('pf_tree: aggregate must look like ''expr AS name'': ' || s) END
-- @@
CREATE OR REPLACE MACRO pf_tree__aggs(aggs) AS
  array_to_string(list_transform(pf_tree__to_list(aggs), a -> ', ' || pf_tree__agg_expr(a) || ' AS "__pf_agg_' || pf_tree__agg_name(a) || '"'), '')
-- @@
CREATE OR REPLACE MACRO pf_tree__agg_out(aggs) AS
  array_to_string(list_transform(pf_tree__to_list(aggs), a -> ', "__pf_agg_' || pf_tree__agg_name(a) || '" AS ' || pf_tree__agg_name(a)), '')
-- @@
CREATE OR REPLACE MACRO pf_tree__agg_names(aggs) AS
  list_transform(pf_tree__to_list(aggs), a -> pf_tree__agg_name(a))
-- @@
CREATE OR REPLACE MACRO pf_tree__dir(direction) AS
  CASE lower(direction) WHEN 'up' THEN '0' WHEN 'down' THEN '1'
       ELSE error('pf_tree: direction must be ''up'' or ''down''') END

-- 6. TREE ACCUMULATE ----------------------------------------------------------
-- @@
CREATE OR REPLACE MACRO pf_tree__acc_fn(a) AS
  upper(regexp_extract(pf_tree__agg_expr(a), '^\s*(\w+)\s*\(', 1))
-- @@
CREATE OR REPLACE MACRO pf_tree__acc_args(a) AS
  trim(regexp_extract(pf_tree__agg_expr(a), '^\s*\w+\s*\((.*)\)\s*$', 1))
-- @@
CREATE OR REPLACE MACRO pf_tree__arg1(x) AS
  CASE WHEN regexp_matches(x, ',') THEN regexp_extract(x, '^([^,]*),(.*)$', 1) ELSE x END
-- @@
CREATE OR REPLACE MACRO pf_tree__arg2(x) AS
  CASE WHEN regexp_matches(x, ',') THEN regexp_extract(x, '^([^,]*),(.*)$', 2) ELSE NULL END
-- @@
CREATE OR REPLACE MACRO pf_tree__acc_kernel(a, i, d) AS
  CASE pf_tree__acc_fn(a)
    WHEN 'SUM' THEN ', unnest(pf_tree_accumulate_agg(id, parent_id, ' || pf_tree__acc_args(a) || ', ' || d || ', 0)) AS __pf_x' || i
    WHEN 'MIN' THEN ', unnest(pf_tree_accumulate_agg(id, parent_id, ' || pf_tree__acc_args(a) || ', ' || d || ', 1)) AS __pf_x' || i
    WHEN 'MAX' THEN ', unnest(pf_tree_accumulate_agg(id, parent_id, ' || pf_tree__acc_args(a) || ', ' || d || ', 2)) AS __pf_x' || i
    WHEN 'COUNT' THEN CASE WHEN pf_tree__acc_args(a) = '*'
      THEN ', unnest(pf_tree_accumulate_agg(id, parent_id, 0::BIGINT, ' || d || ', 3)) AS __pf_x' || i
      ELSE ', unnest(pf_tree_accumulate_agg(id, parent_id, CASE WHEN (' || pf_tree__acc_args(a)
           || ') IS NULL THEN NULL ELSE 0 END::BIGINT, ' || d || ', 4)) AS __pf_x' || i END
    WHEN 'AVG' THEN ', unnest(pf_tree_accumulate_agg(id, parent_id, (' || pf_tree__acc_args(a) || ')::DOUBLE, ' || d || ', 0)) AS __pf_x' || i
                 || ', unnest(pf_tree_accumulate_agg(id, parent_id, (' || pf_tree__acc_args(a) || ')::DOUBLE, ' || d || ', 4)) AS __pf_c' || i
    WHEN 'ARG_MIN' THEN ', unnest(pf_tree_accumulate_id_agg(id, parent_id, ' || pf_tree__arg2(pf_tree__acc_args(a)) || ', ' || d || ', 5)) AS __pf_x' || i
    WHEN 'ARG_MAX' THEN ', unnest(pf_tree_accumulate_id_agg(id, parent_id, ' || pf_tree__arg2(pf_tree__acc_args(a)) || ', ' || d || ', 6)) AS __pf_x' || i
    WHEN 'FIRST' THEN ', unnest(pf_tree_accumulate_id_agg(id, parent_id, 0::BIGINT, ' || d || ', 7)) AS __pf_x' || i
    WHEN 'LAST' THEN ', unnest(pf_tree_accumulate_id_agg(id, parent_id, 0::BIGINT, ' || d || ', 8)) AS __pf_x' || i
    WHEN 'GROUP_CONCAT' THEN ''
    ELSE error('pf_tree_accumulate: unsupported aggregate: ' || a) END
-- @@
-- Extra CTEs: the value of the chosen node (ARG_*, FIRST, LAST) and
-- GROUP_CONCAT strings (UP: members in id order, DOWN: root -> node).
CREATE OR REPLACE MACRO pf_tree__acc_cte(a, i, d, tbl, per) AS
  CASE
    WHEN pf_tree__acc_fn(a) IN ('ARG_MIN', 'ARG_MAX', 'FIRST', 'LAST')
      THEN ', __pf_v' || i || ' AS (SELECT ' || pf_tree__k(per) || 'id::BIGINT AS __pf_vid, ('
           || pf_tree__arg1(pf_tree__acc_args(a)) || ') AS __pf_val FROM ' || pf_tree__src(tbl) || ')'
    WHEN pf_tree__acc_fn(a) = 'GROUP_CONCAT'
      THEN ', __pf_v' || i || ' AS (SELECT ' || pf_tree__k(per) || 'id::BIGINT AS id, ('
           || pf_tree__arg1(pf_tree__acc_args(a)) || ')::VARCHAR AS __pf_val FROM ' || pf_tree__src(tbl) || ')'
           || ', __pf_p' || i || ' AS (SELECT ' || pf_tree__k(per)
           || 'unnest(pf_tree_ancestors_agg(id, parent_id, true), recursive := true) FROM ' || pf_tree__src(tbl)
           || pf_tree__grp(per) || ' UNION ALL SELECT ' || pf_tree__k(per) || 'id, id AS aid, 0 AS dist FROM ' || pf_tree__src(tbl) || ')'
           || ', __pf_s' || i || ' AS (SELECT ' || array_to_string(list_transform(pf_tree__to_list(per), c -> '__pf_p.' || c || ', '), '')
           || CASE WHEN d = '0' THEN '__pf_p.aid' ELSE '__pf_p.id' END || ' AS __pf_sid, string_agg(__pf_v.__pf_val, '
           || coalesce(pf_tree__arg2(pf_tree__acc_args(a)), ''',''') || ' ORDER BY '
           || CASE WHEN d = '0' THEN '__pf_p.id' ELSE '__pf_p.dist DESC' END
           || ') AS __pf_str FROM __pf_p' || i || ' __pf_p JOIN __pf_v' || i || ' __pf_v ON '
           || pf_tree__on(per, '__pf_v', '__pf_p') || '__pf_v.id = '
           || CASE WHEN d = '0' THEN '__pf_p.id' ELSE '__pf_p.aid' END || ' GROUP BY ALL)'
    ELSE '' END
-- @@
CREATE OR REPLACE MACRO pf_tree__acc_join(a, i, per) AS
  CASE
    WHEN pf_tree__acc_fn(a) IN ('ARG_MIN', 'ARG_MAX', 'FIRST', 'LAST')
      THEN ' LEFT JOIN __pf_v' || i || ' ON ' || pf_tree__on(per, '__pf_v' || i, '__pf_t')
           || '__pf_v' || i || '.__pf_vid = __pf_k.__pf_x' || i || '.v'
    WHEN pf_tree__acc_fn(a) = 'GROUP_CONCAT'
      THEN ' LEFT JOIN __pf_s' || i || ' ON ' || pf_tree__on(per, '__pf_s' || i, '__pf_t')
           || '__pf_s' || i || '.__pf_sid = __pf_t.id'
    ELSE '' END
-- @@
CREATE OR REPLACE MACRO pf_tree__acc_out(a, i) AS
  CASE pf_tree__acc_fn(a)
    WHEN 'AVG' THEN ', __pf_k.__pf_x' || i || '.v / NULLIF(__pf_k.__pf_c' || i || '.v, 0) AS ' || pf_tree__agg_name(a)
    WHEN 'ARG_MIN' THEN ', __pf_v' || i || '.__pf_val AS ' || pf_tree__agg_name(a)
    WHEN 'ARG_MAX' THEN ', __pf_v' || i || '.__pf_val AS ' || pf_tree__agg_name(a)
    WHEN 'FIRST' THEN ', __pf_v' || i || '.__pf_val AS ' || pf_tree__agg_name(a)
    WHEN 'LAST' THEN ', __pf_v' || i || '.__pf_val AS ' || pf_tree__agg_name(a)
    WHEN 'GROUP_CONCAT' THEN ', __pf_s' || i || '.__pf_str AS ' || pf_tree__agg_name(a)
    ELSE ', __pf_k.__pf_x' || i || '.v AS ' || pf_tree__agg_name(a) END
-- @@
-- TREE ACCUMULATE (UP|DOWN) agg AS name {, agg AS name}
--   aggs: ['sum(dur) AS total', 'max(depth) AS m', 'arg_max(name, dur) AS a', ...]
-- Row preserving: all input columns + one column per aggregate. UP aggregates
-- each node's subtree (self included), DOWN the root->node path. SUM follows
-- TP: NULL contributes 0, BIGINT overflow is an error. MIN/MAX/AVG/COUNT(x)
-- ignore NULLs; COUNT(*) counts nodes; ARG_MIN/ARG_MAX(arg, val): ties ->
-- smallest id; FIRST/LAST(x) (DOWN only): x at the path's root / the node;
-- GROUP_CONCAT(x[, sep]) (default ','): UP members in id order, DOWN
-- root -> node. Errors (like TP): duplicate id, NULL id, self parent, cycle,
-- parent not in the input.
CREATE OR REPLACE MACRO pf_tree__accumulate_full(tbl, direction, aggs, per := []::VARCHAR[]) AS TABLE
FROM query(
  'WITH __pf_k AS MATERIALIZED (SELECT ' || pf_tree__k(per) || '0 AS __pf_dummy'
  || array_to_string(list_transform(pf_tree__to_list(aggs), (a, i) -> pf_tree__acc_kernel(a, i, pf_tree__dir(direction))), '')
  || ' FROM ' || pf_tree__src(tbl) || pf_tree__grp(per) || ')'
  || array_to_string(list_transform(pf_tree__to_list(aggs), (a, i) -> pf_tree__acc_cte(a, i, pf_tree__dir(direction), tbl, per)), '')
  || ' SELECT __pf_t.*' || array_to_string(list_transform(pf_tree__to_list(aggs), (a, i) -> pf_tree__acc_out(a, i)), '')
  || ' FROM ' || pf_tree__src(tbl) || ' __pf_t JOIN __pf_k ON ' || pf_tree__on(per, '__pf_k', '__pf_t') || '__pf_t.id = '
  || CASE WHEN list_bool_and(list_transform(pf_tree__to_list(aggs), a -> pf_tree__acc_fn(a) = 'GROUP_CONCAT'))
       THEN '__pf_t.id' ELSE '__pf_k.__pf_x' || list_position(list_transform(pf_tree__to_list(aggs), a -> pf_tree__acc_fn(a) <> 'GROUP_CONCAT'), true) || '.id' END
  || array_to_string(list_transform(pf_tree__to_list(aggs), (a, i) -> pf_tree__acc_join(a, i, per)), '')
)
-- @@
-- Exact TP: TREE ACCUMULATE UP SUM(value_col) AS total.
CREATE OR REPLACE MACRO pf_tree_accumulate(tbl, direction, aggs, per := []::VARCHAR[], full_row := true) AS TABLE
FROM query(pf__sql_tree_accumulate(tbl, direction, aggs, per, full_row, current_setting('threads')::BIGINT))
-- @@
CREATE OR REPLACE MACRO pf_tree_accumulate_up(tbl, value_col, per := []::VARCHAR[]) AS TABLE
FROM query(pf__sql_tree_accumulate(tbl, 'up', 'sum(' || value_col || ') AS total', per, false,
                                   current_setting('threads')::BIGINT))
-- @@
-- Exact TP: TREE ACCUMULATE DOWN SUM(value_col) AS total.
CREATE OR REPLACE MACRO pf_tree_accumulate_down(tbl, value_col, per := []::VARCHAR[]) AS TABLE
FROM query(pf__sql_tree_accumulate(tbl, 'down', 'sum(' || value_col || ') AS total', per, false,
                                   current_setting('threads')::BIGINT))
-- @@
-- CTE __pf_mt: structure rows with __pf_mk (is a marker) and __pf_set (in the
-- working set and not a marker).
CREATE OR REPLACE MACRO pf_tree__marked(tbl, marker, per, over) AS
  'WITH __pf_m AS (SELECT DISTINCT ' || pf_tree__k(per) || 'id::BIGINT AS id FROM ' || pf_tree__src(marker) || '), '
  || CASE WHEN over IS NULL THEN ''
     ELSE '__pf_wk AS (SELECT DISTINCT ' || pf_tree__k(per) || 'id::BIGINT AS id FROM ' || pf_tree__src(tbl) || '), ' END
  || '__pf_mt AS (SELECT ' || array_to_string(list_transform(pf_tree__to_list(per), c -> '__pf_s.' || c || ', '), '')
  || '__pf_s.id::BIGINT AS id, __pf_s.parent_id::BIGINT AS parent_id, __pf_m.id IS NOT NULL AS __pf_mk, '
  || CASE WHEN over IS NULL THEN 'true' ELSE '__pf_wk.id IS NOT NULL' END || ' AND __pf_m.id IS NULL AS __pf_set '
  || 'FROM ' || pf_tree__src(coalesce(over, tbl)) || ' __pf_s LEFT JOIN __pf_m ON ' || pf_tree__on(per, '__pf_s', '__pf_m')
  || '__pf_s.id = __pf_m.id'
  || CASE WHEN over IS NULL THEN '' ELSE ' LEFT JOIN __pf_wk ON ' || pf_tree__on(per, '__pf_s', '__pf_wk')
     || '__pf_s.id = __pf_wk.id' END || ') '
-- @@
CREATE OR REPLACE MACRO pf_tree__flag_filter(tbl, marker, per, over, mode, keep) AS
  pf_tree__marked(tbl, marker, per, over)
  || ', __pf_f AS MATERIALIZED (SELECT ' || pf_tree__k(per) || 'unnest(pf_tree_flags_agg(id, parent_id, __pf_mk, '
  || mode || ', false)) AS id FROM __pf_mt' || pf_tree__grp(per) || ') '
  || 'SELECT __pf_t.* FROM ' || pf_tree__src(tbl) || ' __pf_t ' || CASE WHEN keep THEN 'SEMI' ELSE 'ANTI' END
  || ' JOIN __pf_f ON ' || pf_tree__on(per, '__pf_t', '__pf_f') || '__pf_t.id = __pf_f.id'
-- @@
CREATE OR REPLACE MACRO pf_tree__rel_mode(relation) AS
  CASE lower(relation) WHEN 'descendant' THEN '0' WHEN 'ancestor' THEN '1'
       ELSE error('pf_tree: relation must be ''ancestor'' or ''descendant''') END
-- @@
-- TREE KEEP IF (ANCESTOR|DESCENDANT) OF marker [OVER over]: rows that are a
-- strict ancestor / strict descendant of some marker row (marker: relation
-- with id + per cols). With `over`, structure comes from the complete tree
-- `over` and the rows of `tbl` (a filtered working set) are tested.
-- Without `over`, a parent missing from tbl makes its child a root.
CREATE OR REPLACE MACRO pf_tree_keep_if(tbl, relation, marker, per := []::VARCHAR[], over := NULL, full_row := true) AS TABLE
FROM query(pf__sql_tree_filter('keep_' || relation, tbl, marker, per, over, full_row, current_setting('threads')::BIGINT))
-- @@
-- TREE DROP IF (ANCESTOR|DESCENDANT) OF marker [OVER over]: complement of KEEP IF.
CREATE OR REPLACE MACRO pf_tree_drop_if(tbl, relation, marker, per := []::VARCHAR[], over := NULL, full_row := true) AS TABLE
FROM query(pf__sql_tree_filter('drop_' || relation, tbl, marker, per, over, full_row, current_setting('threads')::BIGINT))
-- @@
-- TREE PRUNE AT marker [OVER over]: drop each marker node and its whole subtree.
CREATE OR REPLACE MACRO pf_tree_prune(tbl, marker, per := []::VARCHAR[], over := NULL, full_row := true) AS TABLE
FROM query(pf__sql_tree_filter('prune', tbl, marker, per, over, full_row, current_setting('threads')::BIGINT))
-- @@
-- TREE REPARENT TO NEAREST ANCESTOR IN marker [OVER over]: row preserving;
-- parent_id becomes the nearest strict ancestor that is a marker (NULL if none).
CREATE OR REPLACE MACRO pf_tree_reparent(tbl, marker, per := []::VARCHAR[], over := NULL) AS TABLE
FROM query(
  pf_tree__marked(tbl, marker, per, over)
  || ', __pf_f AS MATERIALIZED (SELECT ' || pf_tree__k(per) || 'unnest(pf_tree_nearest_agg(id, parent_id, __pf_mk, 0, false), '
  || 'recursive := true) FROM __pf_mt' || pf_tree__grp(per) || ') '
  || 'SELECT __pf_t.* REPLACE (__pf_f.v AS parent_id) FROM ' || pf_tree__src(tbl) || ' __pf_t JOIN __pf_f ON '
  || pf_tree__on(per, '__pf_t', '__pf_f') || '__pf_t.id = __pf_f.id'
)
-- @@
CREATE OR REPLACE MACRO pf_tree__is_sum_cnt(aggs) AS
  (len(pf_tree__to_list(aggs)) = 1 AND pf_tree__acc_fn(pf_tree__to_list(aggs)[1]) = 'SUM')
  OR (len(pf_tree__to_list(aggs)) = 2 AND pf_tree__acc_fn(pf_tree__to_list(aggs)[1]) = 'SUM'
      AND pf_tree__acc_fn(pf_tree__to_list(aggs)[2]) = 'COUNT' AND pf_tree__acc_args(pf_tree__to_list(aggs)[2]) = '*')
-- @@
-- TREE CONTRACT AT marker [OVER over] AGGREGATE aggs: remove each marker node,
-- reparent its children to the nearest surviving ancestor and charge its
-- rows to that ancestor: a survivor's aggregates run over itself plus the
-- removed rows charged to it (rows under a removed root are discarded).
-- Output: surviving rows (columns named like an aggregate are replaced),
-- parent_id rewritten (moved last), then the aggregates.
CREATE OR REPLACE MACRO pf_tree_contract(tbl, marker, aggs, per := []::VARCHAR[], over := NULL, full_row := false) AS TABLE
FROM query(
  CASE WHEN NOT full_row AND over IS NULL AND pf_tree__is_sum_cnt(aggs) THEN
    'WITH __pf_m AS (SELECT DISTINCT ' || pf_tree__k(per) || 'id::BIGINT AS id FROM ' || pf_tree__src(marker) || '), '
    || '__pf_mt AS (SELECT ' || array_to_string(list_transform(pf_tree__to_list(per), c -> '__pf_s.' || c || ', '), '')
    || '__pf_s.id::BIGINT AS id, __pf_s.parent_id::BIGINT AS parent_id, __pf_m.id IS NULL AS __pf_set, ('
    || pf_tree__acc_args(pf_tree__to_list(aggs)[1]) || ')::BIGINT AS __pf_val FROM ' || pf_tree__src(tbl)
    || ' __pf_s LEFT JOIN __pf_m ON ' || pf_tree__on(per, '__pf_s', '__pf_m') || '__pf_s.id = __pf_m.id) '
    || 'SELECT ' || pf_tree__k(per) || 'id, parent_id, v AS ' || pf_tree__agg_name(pf_tree__to_list(aggs)[1])
    || CASE WHEN len(pf_tree__to_list(aggs)) = 2 THEN ', n AS ' || pf_tree__agg_name(pf_tree__to_list(aggs)[2]) ELSE '' END
    || ' FROM (SELECT ' || pf_tree__k(per)
    || 'unnest(pf_tree_contract_sum_agg(id, parent_id, __pf_set, __pf_val, false), recursive := true) FROM __pf_mt'
    || pf_tree__grp(per) || ')'
  ELSE
    pf_tree__marked(tbl, marker, per, over)
    || ', __pf_f AS MATERIALIZED (SELECT ' || pf_tree__k(per) || 'unnest(pf_tree_contract_agg(id, parent_id, __pf_set, false), '
    || 'recursive := true) FROM __pf_mt' || pf_tree__grp(per) || '), '
    || '__pf_w AS (SELECT __pf_t.*, __pf_f.owner AS __pf_owner, __pf_f.parent_id AS __pf_up FROM '
    || pf_tree__src(tbl) || ' __pf_t JOIN __pf_f ON ' || pf_tree__on(per, '__pf_t', '__pf_f') || '__pf_t.id = __pf_f.id) '
    || 'SELECT any_value(COLUMNS(c -> NOT starts_with(c, ''__pf_'') AND c <> ''parent_id'' AND NOT list_contains('
    || pf_tree__strlist(pf_tree__agg_names(aggs)) || ', c))) FILTER (WHERE id = __pf_owner), '
    || 'any_value(__pf_up) FILTER (WHERE id = __pf_owner) AS parent_id'
    || array_to_string(list_transform(pf_tree__to_list(aggs), a -> ', ' || a), '')
    || ' FROM __pf_w GROUP BY ' || pf_tree__k(per) || '__pf_owner'
  END
)
-- @@
-- TREE ABSORB AT marker [OVER over] AGGREGATE aggs: keep each marker node, drop
-- its subtree and fold the subtree's rows into it (nested markers: the
-- topmost absorbs). Other rows keep aggregates over themselves only.
CREATE OR REPLACE MACRO pf_tree_absorb(tbl, marker, aggs, per := []::VARCHAR[], over := NULL, full_row := false) AS TABLE
FROM query(
  CASE WHEN NOT full_row AND over IS NULL AND pf_tree__is_sum_cnt(aggs) THEN
    'WITH __pf_m AS (SELECT DISTINCT ' || pf_tree__k(per) || 'id::BIGINT AS id FROM ' || pf_tree__src(marker) || '), '
    || '__pf_mt AS (SELECT ' || array_to_string(list_transform(pf_tree__to_list(per), c -> '__pf_s.' || c || ', '), '')
    || '__pf_s.id::BIGINT AS id, __pf_s.parent_id::BIGINT AS parent_id, __pf_m.id IS NOT NULL AS __pf_mk, ('
    || pf_tree__acc_args(pf_tree__to_list(aggs)[1]) || ')::BIGINT AS __pf_val FROM ' || pf_tree__src(tbl)
    || ' __pf_s LEFT JOIN __pf_m ON ' || pf_tree__on(per, '__pf_s', '__pf_m') || '__pf_s.id = __pf_m.id) '
    || 'SELECT ' || pf_tree__k(per) || 'id, parent_id, v AS ' || pf_tree__agg_name(pf_tree__to_list(aggs)[1])
    || CASE WHEN len(pf_tree__to_list(aggs)) = 2 THEN ', n AS ' || pf_tree__agg_name(pf_tree__to_list(aggs)[2]) ELSE '' END
    || ' FROM (SELECT ' || pf_tree__k(per)
    || 'unnest(pf_tree_absorb_sum_agg(id, parent_id, __pf_mk, __pf_val, false), recursive := true) FROM __pf_mt'
    || pf_tree__grp(per) || ')'
  ELSE
    pf_tree__marked(tbl, marker, per, over)
    || ', __pf_f AS MATERIALIZED (SELECT ' || pf_tree__k(per) || 'unnest(pf_tree_nearest_agg(id, parent_id, __pf_mk, 1, false), '
    || 'recursive := true) FROM __pf_mt' || pf_tree__grp(per) || '), '
    || '__pf_w AS (SELECT __pf_t.*, coalesce(__pf_f.v, __pf_t.id::BIGINT) AS __pf_owner FROM '
    || pf_tree__src(tbl) || ' __pf_t JOIN __pf_f ON ' || pf_tree__on(per, '__pf_t', '__pf_f') || '__pf_t.id = __pf_f.id) '
    || 'SELECT any_value(COLUMNS(c -> NOT starts_with(c, ''__pf_'') AND NOT list_contains('
    || pf_tree__strlist(pf_tree__agg_names(aggs)) || ', c))) FILTER (WHERE id = __pf_owner)'
    || array_to_string(list_transform(pf_tree__to_list(aggs), a -> ', ' || a), '')
    || ' FROM __pf_w GROUP BY ' || pf_tree__k(per) || '__pf_owner'
  END
)

-- Merges & Keying -------------------------------------------------------------
-- @@
CREATE OR REPLACE MACRO pf_tree__keyhash(keys) AS
  '(hash(' || array_to_string(pf_tree__to_list(keys), ', ') || ') & 9223372036854775807)::BIGINT'
-- @@
-- TREE FLATTEN RECURSION BY keys: row-preserving; a node that agrees with its
-- parent on `keys` becomes its parent's sibling, keeping its own subtree
-- (repeatedly, so a chain A -> A -> A becomes three siblings, and a same-key
-- child of a root becomes a root).
CREATE OR REPLACE MACRO pf_tree_flatten_recursion(tbl, keys, per := []::VARCHAR[], over := NULL) AS TABLE
FROM query(
  'WITH __pf_f AS MATERIALIZED (SELECT ' || pf_tree__k(per)
  || 'unnest(pf_tree_flatten_recursion_agg(id, parent_id, ' || pf_tree__keyhash(keys)
  || '), recursive := true) FROM ' || pf_tree__src(coalesce(over, tbl)) || pf_tree__grp(per) || ') '
  || 'SELECT __pf_t.* REPLACE (__pf_f.parent_id AS parent_id) FROM ' || pf_tree__src(tbl) || ' __pf_t JOIN __pf_f ON '
  || pf_tree__on(per, '__pf_t', '__pf_f') || '__pf_t.id = __pf_f.id'
)
-- @@
-- TREE KEY SIBLINGS BY keys AS (key_col, parent_key_col): row-preserving;
-- adds dense path keys `key_col` and `parent_key_col` (NULL for roots) to
-- every node, where two nodes get the same key when their parents have the
-- same key and they agree on `keys`.
CREATE OR REPLACE MACRO pf_tree_key_siblings(tbl, keys, key_col := 'node_id', parent_key_col := 'parent_node_id',
                                             per := []::VARCHAR[], ordered := false) AS TABLE
FROM query(
  'WITH __pf_f AS MATERIALIZED (SELECT ' || pf_tree__k(per) || 'unnest(pf_tree_merge_siblings_agg(id, parent_id, '
  || pf_tree__keyhash(keys) || ', ' || CASE WHEN ordered THEN 'true' ELSE 'false' END
  || '), recursive := true) FROM ' || pf_tree__src(tbl) || pf_tree__grp(per) || ') '
  || 'SELECT __pf_t.*, __pf_f.mid AS ' || key_col || ', __pf_f.pmid AS ' || parent_key_col
  || ' FROM ' || pf_tree__src(tbl) || ' __pf_t JOIN __pf_f ON ' || pf_tree__on(per, '__pf_t', '__pf_f') || '__pf_t.id = __pf_f.id'
)
-- @@
-- TREE MERGE SIBLINGS BY keys [ORDERED] AGGREGATE aggs: re-key every node by
-- its root-to-node path of `keys` and unify nodes with equal paths (callstack
-- -> flamegraph). ORDERED unifies only consecutive siblings (in id order).
-- Output: per cols, id (new, dense per partition), parent_id (new), the key
-- columns, the aggregates over the merged rows.
CREATE OR REPLACE MACRO pf_tree_merge_siblings(tbl, keys, aggs, per := []::VARCHAR[], ordered := false) AS TABLE
FROM query(
  CASE WHEN pf_tree__is_sum_cnt(aggs) THEN
    'WITH __pf_f AS MATERIALIZED (SELECT ' || pf_tree__k(per)
    || 'unnest(pf_tree_merge_siblings_sum_agg(id, parent_id, ' || pf_tree__keyhash(keys)
    || ', (' || pf_tree__acc_args(pf_tree__to_list(aggs)[1]) || ')::BIGINT, '
    || CASE WHEN ordered THEN 'true' ELSE 'false' END
    || '), recursive := true) FROM ' || pf_tree__src(tbl) || pf_tree__grp(per) || ') '
    || 'SELECT ' || array_to_string(list_transform(pf_tree__to_list(per), c -> '__pf_f.' || c || ', '), '')
    || '__pf_f.id, __pf_f.parent_id'
    || array_to_string(list_transform(pf_tree__to_list(keys), k -> ', __pf_t.' || k), '')
    || ', __pf_f.v AS ' || pf_tree__agg_name(pf_tree__to_list(aggs)[1])
    || CASE WHEN len(pf_tree__to_list(aggs)) = 2
         THEN ', __pf_f.n AS ' || pf_tree__agg_name(pf_tree__to_list(aggs)[2])
         ELSE '' END
    || ' FROM __pf_f JOIN ' || pf_tree__src(tbl) || ' __pf_t ON '
    || pf_tree__on(per, '__pf_t', '__pf_f') || '__pf_t.id = __pf_f.rep_id'
  ELSE
    'WITH __pf_f AS MATERIALIZED (SELECT ' || pf_tree__k(per) || 'unnest(pf_tree_merge_siblings_agg(id, parent_id, '
    || pf_tree__keyhash(keys) || ', ' || CASE WHEN ordered THEN 'true' ELSE 'false' END
    || '), recursive := true) FROM ' || pf_tree__src(tbl) || pf_tree__grp(per) || '), '
    || '__pf_w AS (SELECT __pf_t.*, __pf_f.mid AS __pf_mid, __pf_f.pmid AS __pf_pmid FROM '
    || pf_tree__src(tbl) || ' __pf_t JOIN __pf_f ON ' || pf_tree__on(per, '__pf_t', '__pf_f') || '__pf_t.id = __pf_f.id) '
    || 'SELECT ' || pf_tree__k(per) || '__pf_mid AS id, any_value(__pf_pmid) AS parent_id'
    || array_to_string(list_transform(pf_tree__to_list(keys), k -> ', any_value(' || k || ') AS ' || k), '')
    || array_to_string(list_transform(pf_tree__to_list(aggs), a -> ', ' || a), '')
    || ' FROM __pf_w GROUP BY ' || pf_tree__k(per) || '__pf_mid'
  END
)
-- @@
-- TREE MERGE INTO PARENT BY keys [INDIRECT] AGGREGATE aggs: a node whose key
-- equals its parent's is collapsed into the parent (chains collapse into the
-- topmost). INDIRECT: a node whose key occurs anywhere on its root path folds
-- itself and every node between it and the outermost occurrence into that
-- occurrence. Output: surviving rows (parent_id rewritten, moved last) and the
-- aggregates over the rows folded into each survivor (self included).
CREATE OR REPLACE MACRO pf_tree_merge_into_parent(tbl, keys, aggs, per := []::VARCHAR[], indirect := false) AS TABLE
FROM query(
  CASE WHEN pf_tree__is_sum_cnt(aggs) THEN
    'WITH __pf_f AS MATERIALIZED (SELECT '
    || array_to_string(list_transform(pf_tree__to_list(per), (c, i) -> c || ' AS __pf_p' || i || ', '), '')
    || 'f.id AS __pf_id, f.parent_id AS __pf_np, f.v AS __pf_v, f.n AS __pf_n FROM (SELECT ' || pf_tree__k(per)
    || 'unnest(pf_tree_merge_into_parent_sum_agg(id, parent_id, ' || pf_tree__keyhash(keys)
    || ', (' || pf_tree__acc_args(pf_tree__to_list(aggs)[1]) || ')::BIGINT, '
    || CASE WHEN indirect THEN 'true' ELSE 'false' END
    || ')) AS f FROM ' || pf_tree__src(tbl) || pf_tree__grp(per) || ')) '
    || 'SELECT COLUMNS(c -> NOT starts_with(c, ''__pf_'') AND c <> ''parent_id'' AND NOT list_contains('
    || pf_tree__strlist(pf_tree__agg_names(aggs)) || ', c)), __pf_f.__pf_np AS parent_id, '
    || '__pf_f.__pf_v AS ' || pf_tree__agg_name(pf_tree__to_list(aggs)[1])
    || CASE WHEN len(pf_tree__to_list(aggs)) = 2
         THEN ', __pf_f.__pf_n AS ' || pf_tree__agg_name(pf_tree__to_list(aggs)[2])
         ELSE '' END
    || ' FROM __pf_f JOIN ' || pf_tree__src(tbl) || ' __pf_t ON '
    || array_to_string(list_transform(pf_tree__to_list(per), (c, i) -> '__pf_t.' || c || ' = __pf_f.__pf_p' || i || ' AND '), '')
    || '__pf_t.id = __pf_f.__pf_id'
  ELSE
    'WITH __pf_f AS MATERIALIZED (SELECT ' || pf_tree__k(per) || 'unnest(pf_tree_merge_into_parent_agg(id, parent_id, '
    || pf_tree__keyhash(keys) || ', ' || CASE WHEN indirect THEN 'true' ELSE 'false' END
    || '), recursive := true) FROM ' || pf_tree__src(tbl) || pf_tree__grp(per) || '), '
    || '__pf_w AS (SELECT __pf_t.*, __pf_f.owner AS __pf_owner, __pf_f.parent_id AS __pf_np FROM '
    || pf_tree__src(tbl) || ' __pf_t JOIN __pf_f ON ' || pf_tree__on(per, '__pf_t', '__pf_f') || '__pf_t.id = __pf_f.id) '
    || 'SELECT any_value(COLUMNS(c -> NOT starts_with(c, ''__pf_'') AND c <> ''parent_id'' AND NOT list_contains('
    || pf_tree__strlist(pf_tree__agg_names(aggs)) || ', c))) FILTER (WHERE id = __pf_owner), '
    || 'any_value(__pf_np) FILTER (WHERE id = __pf_owner) AS parent_id'
    || array_to_string(list_transform(pf_tree__to_list(aggs), a -> ', ' || a), '')
    || ' FROM __pf_w GROUP BY ' || pf_tree__k(per) || '__pf_owner'
  END
)
-- @@
-- TREE INVERT: every leaf becomes a root and each root->leaf path is reversed;
-- a node appears once per leaf below it. Output: per cols, id, parent_id (new,
-- dense per partition), node_id (original id), leaf_id, then the original
-- node's other columns. Usually followed by pf_tree_merge_siblings.
CREATE OR REPLACE MACRO pf_tree_invert(tbl, per := []::VARCHAR[]) AS TABLE
FROM query(
  'WITH __pf_f AS MATERIALIZED (SELECT ' || pf_tree__k(per) || 'unnest(pf_tree_invert_agg(id, parent_id), recursive := true) FROM '
  || pf_tree__src(tbl) || pf_tree__grp(per) || ') '
  || 'SELECT __pf_f.*, __pf_t.* EXCLUDE (' || pf_tree__k(per) || 'id, parent_id) FROM __pf_f JOIN '
  || pf_tree__src(tbl) || ' __pf_t ON ' || pf_tree__on(per, '__pf_t', '__pf_f') || '__pf_t.id = __pf_f.node_id'
)

-- TREE EXPAND -------------------------------------------------------------------
-- @@
-- TREE EXPAND (UP|DOWN) operand BY keys [ORDERED BY ordered_by]
--             [CHARGE charge TO (LEAF|ROOT)]
-- For every node matching operand rows on `keys` (shared column names), insert
-- a copy of the operand's sub-structure: rows with their own local id /
-- parent_id (NULL = top), or, with ordered_by, a chain in that order.
--   DOWN: the copy's tops hang below the node; the node's existing children
--         reconnect to the copy's single leaf (they stay on the node if the
--         copy fans out). CHARGE ... TO LEAF moves the node's `charge` columns
--         to that leaf (NULL on the node); needs a single leaf.
--   UP:   the copy is inserted between the node and its parent: tops hang
--         below the old parent, the node below the copy's single leaf (error
--         otherwise). CHARGE ... TO ROOT moves `charge` to the single top.
-- DOWN pairs only with LEAF, UP only with ROOT. New ids continue after the
-- partition's max(id). Output: original rows (parent_id / charge rewritten)
-- UNION ALL BY NAME inserted rows (per cols, id, parent_id, keys, operand
-- payload, expanded_from = matched node id).
CREATE OR REPLACE MACRO pf_tree_expand(tbl, direction, operand, keys, ordered_by := []::VARCHAR[],
                                       charge := []::VARCHAR[], charge_to := NULL,
                                       per := []::VARCHAR[]) AS TABLE
FROM query(
  CASE
    WHEN charge_to IS NOT NULL AND lower(direction) = 'down' AND lower(charge_to) <> 'leaf'
      THEN error('pf_tree_expand: EXPAND DOWN can only CHARGE ... TO LEAF')
    WHEN charge_to IS NOT NULL AND lower(direction) = 'up' AND lower(charge_to) <> 'root'
      THEN error('pf_tree_expand: EXPAND UP can only CHARGE ... TO ROOT')
    WHEN lower(direction) NOT IN ('up', 'down')
      THEN error('pf_tree_expand: direction must be ''up'' or ''down''')
    WHEN len(pf_tree__to_list(charge)) > 0 AND charge_to IS NULL
      THEN error('pf_tree_expand: charge needs charge_to')
    ELSE '' END
  || 'WITH __pf_mx AS (SELECT ' || pf_tree__k(per) || 'max(id)::BIGINT AS __pf_base FROM ' || pf_tree__src(tbl) || pf_tree__grp(per) || '), '
  || '__pf_o0 AS (SELECT *, '
  || CASE WHEN len(pf_tree__to_list(ordered_by)) > 0
       THEN 'row_number() OVER (PARTITION BY ' || array_to_string(pf_tree__to_list(keys), ', ') || ' ORDER BY '
            || array_to_string(pf_tree__to_list(ordered_by), ', ') || ')::BIGINT AS __pf_lid'
       ELSE 'id::BIGINT AS __pf_lid, parent_id::BIGINT AS __pf_lpid' END
  || ' FROM ' || pf_tree__src(operand) || '), '
  || '__pf_o AS (SELECT COLUMNS(c -> NOT list_contains(' || pf_tree__strlist(list_concat(pf_tree__to_list(per), ['id', 'parent_id'])) || ', c))'
  || CASE WHEN len(pf_tree__to_list(ordered_by)) > 0 THEN ', CASE WHEN __pf_lid = 1 THEN NULL ELSE __pf_lid - 1 END AS __pf_lpid' ELSE '' END
  || ' FROM __pf_o0), '
  || '__pf_i AS (SELECT ' || array_to_string(list_transform(pf_tree__to_list(per), c -> '__pf_n.' || c || ', '), '')
  || '__pf_n.id::BIGINT AS __pf_node, __pf_n.parent_id::BIGINT AS __pf_nparent, __pf_o.*, '
  || '__pf_mx.__pf_base + row_number() OVER (' || CASE WHEN len(pf_tree__to_list(per)) = 0 THEN '' ELSE 'PARTITION BY '
  || array_to_string(list_transform(pf_tree__to_list(per), c -> '__pf_n.' || c), ', ') END
  || ' ORDER BY __pf_n.id, __pf_o.__pf_lid) AS __pf_new FROM ' || pf_tree__src(tbl) || ' __pf_n JOIN __pf_o ON '
  || array_to_string(list_transform(pf_tree__to_list(keys), k -> '__pf_n.' || k || ' = __pf_o.' || k), ' AND ')
  || CASE WHEN len(pf_tree__to_list(per)) = 0 THEN ' CROSS JOIN __pf_mx' ELSE ' JOIN __pf_mx ON '
     || array_to_string(list_transform(pf_tree__to_list(per), c -> '__pf_n.' || c || ' = __pf_mx.' || c), ' AND ') END || '), '
  || '__pf_e AS (SELECT ' || pf_tree__k(per) || '__pf_node, '
  || 'count(*) FILTER (WHERE NOT __pf_haskid) AS __pf_nleaf, min(__pf_new) FILTER (WHERE NOT __pf_haskid) AS __pf_leaf, '
  || 'count(*) FILTER (WHERE __pf_lpid IS NULL) AS __pf_ntop, min(__pf_new) FILTER (WHERE __pf_lpid IS NULL) AS __pf_top '
  || 'FROM (SELECT __pf_i.*, EXISTS (SELECT 1 FROM __pf_i __pf_c WHERE ' || pf_tree__on(per, '__pf_c', '__pf_i')
  || '__pf_c.__pf_node = __pf_i.__pf_node AND __pf_c.__pf_lpid = __pf_i.__pf_lid) AS __pf_haskid FROM __pf_i) '
  || 'GROUP BY ' || pf_tree__k(per) || '__pf_node), '
  || '__pf_ins AS (SELECT __pf_i.* EXCLUDE (__pf_new, __pf_lid, __pf_lpid, __pf_node, __pf_nparent), '
  || '__pf_i.__pf_new AS id, CASE WHEN __pf_i.__pf_lpid IS NULL THEN '
  || CASE WHEN lower(direction) = 'down' THEN '__pf_i.__pf_node' ELSE '__pf_i.__pf_nparent' END
  || ' ELSE __pf_p.__pf_new END AS parent_id, __pf_i.__pf_node AS expanded_from'
  || array_to_string(list_transform(pf_tree__to_list(charge), c -> ', CASE WHEN __pf_i.__pf_new = '
       || CASE WHEN lower(coalesce(charge_to, 'leaf')) = 'leaf' THEN '__pf_e.__pf_leaf' ELSE '__pf_e.__pf_top' END
       || ' THEN __pf_src.' || c || ' END AS ' || c), '')
  || ' FROM __pf_i LEFT JOIN __pf_i __pf_p ON ' || pf_tree__on(per, '__pf_p', '__pf_i')
  || '__pf_p.__pf_node = __pf_i.__pf_node AND __pf_p.__pf_lid = __pf_i.__pf_lpid '
  || 'JOIN __pf_e ON ' || pf_tree__on(per, '__pf_e', '__pf_i') || '__pf_e.__pf_node = __pf_i.__pf_node '
  || 'JOIN ' || pf_tree__src(tbl) || ' __pf_src ON ' || pf_tree__on(per, '__pf_src', '__pf_i') || '__pf_src.id = __pf_i.__pf_node), '
  || '__pf_orig AS (SELECT __pf_t.* REPLACE ('
  || CASE WHEN lower(direction) = 'down'
       THEN 'CASE WHEN __pf_ep.__pf_nleaf = 1 THEN __pf_ep.__pf_leaf ELSE __pf_t.parent_id END AS parent_id'
       ELSE 'CASE WHEN __pf_es.__pf_node IS NULL THEN __pf_t.parent_id WHEN __pf_es.__pf_nleaf = 1 '
            || 'THEN __pf_es.__pf_leaf ELSE error(''pf_tree_expand: EXPAND UP needs a sub-structure with a single bottom node'') END AS parent_id' END
  || array_to_string(list_transform(pf_tree__to_list(charge), c -> ', CASE WHEN __pf_es.__pf_node IS NULL THEN __pf_t.' || c
       || CASE WHEN lower(coalesce(charge_to, 'leaf')) = 'leaf'
            THEN ' WHEN __pf_es.__pf_nleaf <> 1 THEN error(''pf_tree_expand: CHARGE needs a chain (sub-structure fans out)'')'
            ELSE ' WHEN __pf_es.__pf_ntop <> 1 THEN error(''pf_tree_expand: CHARGE needs a chain (sub-structure has several tops)'')' END
       || ' ELSE NULL END AS ' || c), '')
  || ') FROM ' || pf_tree__src(tbl) || ' __pf_t LEFT JOIN __pf_e __pf_ep ON ' || pf_tree__on(per, '__pf_ep', '__pf_t')
  || '__pf_ep.__pf_node = __pf_t.parent_id LEFT JOIN __pf_e __pf_es ON ' || pf_tree__on(per, '__pf_es', '__pf_t')
  || '__pf_es.__pf_node = __pf_t.id) '
  || 'SELECT * FROM __pf_orig UNION ALL BY NAME SELECT * FROM __pf_ins'
)
