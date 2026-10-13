-- Interval operators (spec §5). Owned by the intervals agent.
--
-- Every operator is a table macro pf_interval_<op>(...). Relations are passed
-- as strings: a table / view name or a parenthesized subquery
-- ('(SELECT ... )'). PER columns are a VARCHAR list; include trace_idx first
-- when the data holds many traces. Each macro builds its SQL text and runs it
-- with query(); native kernels (src/intervals.cc) do the sweeps.

-- -- Helpers ------------------------------------------------------------------
CREATE OR REPLACE MACRO pf_compile_pipe(sql, per := []::VARCHAR[]) AS
  pf__sql_pipe(sql, per, current_setting('threads')::BIGINT)
-- @@
CREATE OR REPLACE MACRO pf_pipe(sql, per := []::VARCHAR[]) AS TABLE
FROM query(pf__sql_pipe(sql, per, current_setting('threads')::BIGINT))
-- @@
CREATE OR REPLACE MACRO trace_start(tbl := 'trace_bounds') AS (SELECT min(start_ts)::BIGINT FROM query('SELECT start_ts FROM ' || tbl))
-- @@
CREATE OR REPLACE MACRO trace_end(tbl := 'trace_bounds') AS (SELECT max(end_ts)::BIGINT FROM query('SELECT end_ts FROM ' || tbl))
-- @@
CREATE OR REPLACE MACRO trace_dur(tbl := 'trace_bounds') AS (SELECT (max(end_ts) - min(start_ts))::BIGINT FROM query('SELECT start_ts, end_ts FROM ' || tbl))
-- @@
CREATE OR REPLACE MACRO pf_interval__src(r) AS
  CASE
    WHEN regexp_matches(trim(r), '(?is)\|>') OR regexp_matches(trim(r), '(?is)^\s*\(?\s*(from|interval|intervals|graph)\b')
    THEN '(' || regexp_replace(trim(pf__sql_pipe(r, []::VARCHAR[], current_setting('threads')::BIGINT)), ';\s*$', '') || ')'
    WHEN regexp_matches(trim(r), '(?is)^(select|with|values)\b|^\(')
    THEN '(' || regexp_replace(trim(r), ';\s*$', '') || ')'
    ELSE regexp_replace(trim(r), ';\s*$', '')
  END
-- @@
CREATE OR REPLACE MACRO pf_interval__to_list(x) AS
  CASE
    WHEN x IS NULL THEN []::VARCHAR[]
    WHEN json_type(to_json(x)) = 'ARRAY' THEN from_json(to_json(x), '["VARCHAR"]')
    WHEN trim(x::VARCHAR) = '' THEN []::VARCHAR[]
    ELSE [trim(x::VARCHAR)]
  END
-- @@
CREATE OR REPLACE MACRO pf_interval__aggs(aggs) AS
  array_to_string(pf_interval__to_list(aggs), ', ')
-- @@
-- "p.c1, p.c2, " (trailing separator) or ''.
CREATE OR REPLACE MACRO pf_interval__keys(per, prefix) AS
  array_to_string(list_transform(pf_interval__to_list(per), c -> prefix || c || ', '), '')
-- @@
-- " GROUP BY c1, c2" or ''.
CREATE OR REPLACE MACRO pf_interval__groupby(per) AS
  CASE WHEN len(pf_interval__to_list(per)) = 0 THEN '' ELSE ' GROUP BY ' || array_to_string(pf_interval__to_list(per), ', ') END
-- @@
-- "PARTITION BY c1, c2" or ''.
CREATE OR REPLACE MACRO pf_interval__partition(per) AS
  CASE WHEN len(pf_interval__to_list(per)) = 0 THEN '' ELSE 'PARTITION BY ' || array_to_string(pf_interval__to_list(per), ', ') END
-- @@
-- " AND a.c1 = b.c1 ..." or ''.
CREATE OR REPLACE MACRO pf_interval__on(per, a, b) AS
  array_to_string(list_transform(pf_interval__to_list(per), c -> ' AND ' || a || '.' || c || ' = ' || b || '.' || c), '')
-- @@
-- A relation with its rows numbered (__pf_r) and packed as a struct (__pf_s).
CREATE OR REPLACE MACRO pf_interval__num(rel) AS
  '(SELECT pf_row_id() AS __pf_r, __pf_t AS __pf_s, * FROM ' || pf_interval__src(rel) || ' __pf_t)'
-- @@
-- Kernel input: one operand's rows tagged with an operand index.
CREATE OR REPLACE MACRO pf_interval__feed(per, side, cte) AS
  'SELECT ' || pf_interval__keys(per, '') || side || '::INTEGER AS side, __pf_r AS row, '
  || 'ts::BIGINT AS ts, dur::BIGINT AS dur FROM ' || cte
-- @@
-- Relationship name -> kernel code.
CREATE OR REPLACE MACRO pf_interval__rel(relationship) AS
  coalesce([0, 0, 1, 2, 3, 4, 4][list_position(
             ['overlapping', 'overlapping_bounds', 'covering_begin', 'covering_end',
              'covering_bounds', 'within_bounds', 'within'],
             regexp_replace(lower(trim(relationship)), '[ _]+', '_', 'g'))],
           error('pf_interval: unknown relationship ' || relationship))
-- @@
-- Two-operand kernel call: CTE "a" (current rows) and "b" (operand), result
-- unnested as k(f).
CREATE OR REPLACE MACRO pf_interval__pair(rel, operand, per, agg, extra) AS
  'WITH a AS MATERIALIZED ' || pf_interval__num(rel)
  || ', b AS MATERIALIZED ' || pf_interval__num(operand)
  || ', k AS (SELECT unnest(__pf_a) AS f FROM (SELECT ' || agg || '(side, row, ts, dur' || extra
  || ') AS __pf_a FROM (' || pf_interval__feed(per, 0, 'a') || ' UNION ALL '
  || pf_interval__feed(per, 1, 'b') || ')' || pf_interval__groupby(per) || '))'

-- 5.1 Sources ----------------------------------------------------------------
-- @@
-- INTERVAL INTERSECTION OF (rels AS aliases) PER per. Output: ts, dur, the PER
-- columns, then one STRUCT column per alias holding that operand's id (or the
-- full operand row when full_row := true).
CREATE OR REPLACE MACRO pf_interval_intersection(rels, aliases, per := []::VARCHAR[], full_row := false) AS TABLE
FROM query(pf__sql_interval_intersection(rels, aliases, per, full_row, current_setting('threads')::BIGINT))
-- @@
-- INTERVAL UNION OF (rels AS aliases) PER per: fragments wherever any operand
-- covers; absent operands are NULL structs.
CREATE OR REPLACE MACRO pf_interval_union(rels, aliases, per := []) AS TABLE
FROM query(
  'WITH ' || array_to_string(list_transform(rels, (r, i) -> 'o' || i || ' AS MATERIALIZED ' || pf_interval__num(r)), ', ')
  || ', k AS (SELECT ' || pf_interval__keys(per, '') || 'unnest(__pf_a) AS f FROM (SELECT '
  || pf_interval__keys(per, '') || 'pf_interval_union_agg(side, row, ts, dur, ' || len(rels)
  || ') AS __pf_a FROM ('
  || array_to_string(list_transform(rels, (r, i) -> pf_interval__feed(per, i - 1, 'o' || i)), ' UNION ALL ')
  || ')' || pf_interval__groupby(per) || '))'
  || ' SELECT f.ts AS ts, f.dur AS dur, ' || pf_interval__keys(per, 'k.')
  || array_to_string(list_transform(aliases, (a, i) -> 'o' || i || '.__pf_s AS ' || a), ', ')
  || ' FROM k ' || array_to_string(list_transform(rels, (r, i) ->
       'LEFT JOIN o' || i || ' ON o' || i || '.__pf_r = f.rows[' || i || ']'), ' ')
)
-- @@
-- INTERVALS FROM EVENTS rel PER per [STOP stop] [KEEPING LAST OPEN |
-- CLOSING LAST AT (close_last_at)]. Each event opens an interval closed by
-- the next event in its lane or the first STOP event at or after it. The last
-- open interval is dropped unless keep_last_open (dur = -1) or close_last_at
-- (an expression over the event row) is given.
CREATE OR REPLACE MACRO pf_intervals_from_events(rel, per := [], stop := NULL,
                                                 keep_last_open := false,
                                                 close_last_at := NULL,
                                                 full_row := false) AS TABLE
FROM query(
  CASE WHEN NOT full_row THEN
    'SELECT ' || pf_interval__keys(per, '') || 'f.id AS id, f.ts AS ts, f.dur AS dur FROM ('
    || 'SELECT ' || pf_interval__keys(per, '')
    || 'unnest(pf_intervals_from_events_agg(side, id, ts, close_at, '
    || CASE WHEN keep_last_open THEN '1' WHEN close_last_at IS NOT NULL THEN '2' ELSE '0' END
    || ')) AS f FROM ('
    || 'SELECT ' || pf_interval__keys(per, '') || '0::INTEGER AS side, id::BIGINT AS id, ts::BIGINT AS ts, '
    || COALESCE('(' || close_last_at || ')::BIGINT', '0::BIGINT') || ' AS close_at FROM ' || pf_interval__src(rel) || ' __pf_t'
    || CASE WHEN stop IS NULL THEN ''
       ELSE ' UNION ALL SELECT ' || pf_interval__keys(per, '') || '1::INTEGER AS side, 0::BIGINT AS id, ts::BIGINT AS ts, 0::BIGINT AS close_at FROM ' || pf_interval__src(stop) || ' __pf_u'
       END
    || ')' || pf_interval__groupby(per) || ')'
  ELSE
    'WITH __pf_orig AS MATERIALIZED (SELECT pf_row_id() AS __pf_r, * FROM ' || pf_interval__src(rel) || ' __pf_t), '
    || '__pf_k AS (SELECT ' || pf_interval__keys(per, '')
    || 'unnest(pf_intervals_from_events_agg(side, __pf_r, ts, close_at, '
    || CASE WHEN keep_last_open THEN '1' WHEN close_last_at IS NOT NULL THEN '2' ELSE '0' END
    || ')) AS f FROM ('
    || 'SELECT ' || pf_interval__keys(per, '') || '0::INTEGER AS side, __pf_r::BIGINT AS __pf_r, ts::BIGINT AS ts, '
    || COALESCE('(' || close_last_at || ')::BIGINT', '0::BIGINT') || ' AS close_at FROM __pf_orig'
    || CASE WHEN stop IS NULL THEN ''
       ELSE ' UNION ALL SELECT ' || pf_interval__keys(per, '') || '1::INTEGER AS side, 0::BIGINT AS __pf_r, ts::BIGINT AS ts, 0::BIGINT AS close_at FROM ' || pf_interval__src(stop) || ' __pf_u'
       END
    || ')' || pf_interval__groupby(per) || ') '
    || 'SELECT COLUMNS(c -> c NOT IN (''__pf_r'', ''ts'', ''dur'', ''f'')), f.ts AS ts, f.dur AS dur '
    || 'FROM (SELECT __pf_orig.*, __pf_k.f AS f FROM __pf_k JOIN __pf_orig ON __pf_orig.__pf_r = __pf_k.f.id)'
  END
)

-- 5.2 Reshaping --------------------------------------------------------------
-- @@
-- INTERVAL SPLIT operand AS alias PER per: every current row is cut at the
-- operand's boundaries; each piece carries the operand row (NULL if none).
CREATE OR REPLACE MACRO pf_interval_split(rel, operand, alias, per := []) AS TABLE
FROM query(
  pf_interval__pair(rel, operand, per, 'pf_interval_split_agg', '')
  || ' SELECT a.* EXCLUDE (__pf_r, __pf_s) REPLACE (f.ts AS ts, f.dur AS dur), b.__pf_s AS ' || alias
  || ' FROM k JOIN a ON a.__pf_r = f.r0 LEFT JOIN b ON b.__pf_r = f.r1'
)
-- @@
-- INTERVAL SUBTRACT operand PER per: the parts of each row outside the
-- operand's coverage.
CREATE OR REPLACE MACRO pf_interval_subtract(rel, operand, per := []) AS TABLE
FROM query(
  pf_interval__pair(rel, operand, per, 'pf_interval_subtract_agg', '')
  || ' SELECT a.* EXCLUDE (__pf_r, __pf_s) REPLACE (f.ts AS ts, f.dur AS dur)'
  || ' FROM k JOIN a ON a.__pf_r = f.r0'
)
-- @@
-- INTERVAL FILL GAPS WITH bg PER per: every input row passes through
-- unchanged, and each span of `bg` not covered by the input becomes a filler
-- row carrying that `bg` row's payload (matched by column name; stream columns
-- absent from `bg` are NULL on fillers; `bg` columns absent from the stream
-- are added, NULL on input rows; if `bg` lacks the PER columns, every `bg` row
-- applies to every input lane; if `bg` has the PER columns, lanes with no
-- input rows are still filled).
CREATE OR REPLACE MACRO pf_interval_fill_gaps(rel, bg, per := [], bg_has_per := true) AS TABLE
FROM query(
  CASE
    WHEN bg_has_per IS NULL AND len(pf_interval__to_list(per)) > 0 THEN
      'WITH l AS MATERIALIZED (SELECT DISTINCT 1 AS __pf_1, ' || array_to_string(pf_interval__to_list(per), ', ') || ' FROM ' || pf_interval__src(rel) || '), '
      || 'a0 AS (SELECT * EXCLUDE (__pf_1) FROM l NATURAL RIGHT JOIN (SELECT 1 AS __pf_1, * FROM ' || pf_interval__src(bg) || ' __pf_bg) WHERE ' || pf_interval__to_list(per)[1] || ' IS NOT NULL), '
      || 'a AS MATERIALIZED ' || pf_interval__num('a0')
      || ', b AS MATERIALIZED ' || pf_interval__num(rel)
      || ', k AS (SELECT unnest(__pf_a) AS f FROM (SELECT pf_interval_subtract_agg(side, row, ts, dur) AS __pf_a FROM ('
      || pf_interval__feed(per, 0, 'a') || ' UNION ALL '
      || 'SELECT ' || pf_interval__keys(per, '') || '1::INTEGER AS side, __pf_r AS row, ts::BIGINT AS ts, CASE WHEN dur = -1 THEN 9223372036854775807 - ts::BIGINT ELSE dur::BIGINT END AS dur FROM b'
      || ')' || pf_interval__groupby(per) || '))'
      || ' SELECT * FROM ' || pf_interval__src(rel) || ' __pf_t UNION ALL BY NAME '
      || 'SELECT a.* EXCLUDE (__pf_r, __pf_s) REPLACE (f.ts AS ts, f.dur AS dur) FROM k JOIN a ON a.__pf_r = f.r0 WHERE f.dur > 0'
    WHEN coalesce(bg_has_per, true) THEN
      'WITH a AS MATERIALIZED ' || pf_interval__num(bg)
      || ', b AS MATERIALIZED ' || pf_interval__num(rel)
      || ', k AS (SELECT unnest(__pf_a) AS f FROM (SELECT pf_interval_subtract_agg(side, row, ts, dur) AS __pf_a FROM ('
      || pf_interval__feed(per, 0, 'a') || ' UNION ALL '
      || 'SELECT ' || pf_interval__keys(per, '') || '1::INTEGER AS side, __pf_r AS row, ts::BIGINT AS ts, CASE WHEN dur = -1 THEN 9223372036854775807 - ts::BIGINT ELSE dur::BIGINT END AS dur FROM b'
      || ')' || pf_interval__groupby(per) || '))'
      || ' SELECT * FROM ' || pf_interval__src(rel) || ' __pf_t UNION ALL BY NAME '
      || 'SELECT a.* EXCLUDE (__pf_r, __pf_s) REPLACE (f.ts AS ts, f.dur AS dur) FROM k JOIN a ON a.__pf_r = f.r0 WHERE f.dur > 0'
    ELSE
      'WITH l AS MATERIALIZED (SELECT DISTINCT ' || array_to_string(pf_interval__to_list(per), ', ') || ' FROM ' || pf_interval__src(rel) || '), '
      || 'a0 AS (SELECT l.*, __pf_bg.* FROM l CROSS JOIN ' || pf_interval__src(bg) || ' __pf_bg), '
      || 'a AS MATERIALIZED ' || pf_interval__num('a0')
      || ', b AS MATERIALIZED ' || pf_interval__num(rel)
      || ', k AS (SELECT unnest(__pf_a) AS f FROM (SELECT pf_interval_subtract_agg(side, row, ts, dur) AS __pf_a FROM ('
      || pf_interval__feed(per, 0, 'a') || ' UNION ALL '
      || 'SELECT ' || pf_interval__keys(per, '') || '1::INTEGER AS side, __pf_r AS row, ts::BIGINT AS ts, CASE WHEN dur = -1 THEN 9223372036854775807 - ts::BIGINT ELSE dur::BIGINT END AS dur FROM b'
      || ')' || pf_interval__groupby(per) || '))'
      || ' SELECT * FROM ' || pf_interval__src(rel) || ' __pf_t UNION ALL BY NAME '
      || 'SELECT a.* EXCLUDE (__pf_r, __pf_s) REPLACE (f.ts AS ts, f.dur AS dur) FROM k JOIN a ON a.__pf_r = f.r0 WHERE f.dur > 0'
  END
)
-- @@
-- Alias for pf_interval_fill_within (backward compatibility with earlier draft).
CREATE OR REPLACE MACRO pf_interval_fill_within(rel, within, per := [], bg_has_per := true) AS TABLE
FROM pf_interval_fill_gaps(rel, within, per := per, bg_has_per := bg_has_per)
-- @@
-- INTERVAL FLATTEN PER per AGGREGATE aggs: disjoint segments, rows live in
-- each segment collapsed with the DuckDB aggregate list `aggs` (e.g.
-- 'count(*) AS concurrency, arg_max(name, depth) AS deepest'). TP rules for
-- points (dur = 0). Output: ts, dur, PER columns, aggregates.
CREATE OR REPLACE MACRO pf_interval_flatten(rel, aggs, per := []::VARCHAR[]) AS TABLE
FROM query(pf__sql_interval_flatten(rel, aggs, per, current_setting('threads')::BIGINT))
-- @@
-- INTERVAL FLATTEN fast path for TP's vocabulary: COUNT(*) AS count and
-- SUM(col) AS sum_<col> for every col in `sums`, without exploding segments.
CREATE OR REPLACE MACRO pf_interval_flatten_sum(rel, sums := []::VARCHAR[], per := []::VARCHAR[]) AS TABLE
FROM query(pf__sql_interval_flatten(rel,
  list_concat(['count(*) AS count'], list_transform(pf_interval__to_list(sums), c -> 'sum(' || c || ') AS sum_' || c)),
  per, current_setting('threads')::BIGINT))
-- @@
-- INTERVAL MERGE OVERLAPPING PER per AGGREGATE aggs: coverage of overlapping
-- and abutting rows. Output: ts, dur, PER columns, aggregates.
CREATE OR REPLACE MACRO pf_interval_merge_overlapping(rel, aggs := 'count(*) AS count', per := []) AS TABLE
FROM query(
  CASE WHEN regexp_matches(trim(pf_interval__aggs(aggs)), '^(?i:count\s*\(\s*\*\s*\)\s+as\s+(\w+))$')
  THEN 'SELECT f.ts AS ts, f.dur AS dur, ' || pf_interval__keys(per, '') || 'f.cnt AS '
       || regexp_extract(trim(pf_interval__aggs(aggs)), '^(?i:count\s*\(\s*\*\s*\)\s+as\s+(\w+))$', 1)
       || ' FROM (SELECT ' || pf_interval__keys(per, '')
       || 'unnest(pf_interval_merge_count_agg(ts::BIGINT, dur::BIGINT)) AS f FROM '
       || pf_interval__src(rel) || ' __pf_t' || pf_interval__groupby(per) || ')'
  ELSE 'WITH o AS MATERIALIZED (SELECT pf_row_id() AS __pf_r, * FROM ' || pf_interval__src(rel) || ' __pf_t), '
       || 's AS (SELECT unnest(__pf_a) AS f FROM (SELECT pf_interval_merge_agg(0, __pf_r, ts::BIGINT, dur::BIGINT) AS __pf_a FROM o'
       || pf_interval__groupby(per) || ')), '
       || 'x AS (SELECT f.ts AS __pf_ts, f.dur AS __pf_dur, unnest(f.rows) AS __pf_r FROM s) '
       || 'SELECT __pf_ts AS ts, __pf_dur AS dur, ' || pf_interval__keys(per, '') || pf_interval__aggs(aggs)
       || ' FROM x JOIN o USING (__pf_r) GROUP BY ' || pf_interval__keys(per, '') || '__pf_ts, __pf_dur'
  END
)
-- @@
-- INTERVAL MERGE CONSECUTIVE BY cols PER per AGGREGATE aggs: merge runs of
-- rows (in ts order within the lane) that agree on `by` and touch or overlap
-- the previous row. Output: ts, dur, PER columns, BY columns, aggregates.
CREATE OR REPLACE MACRO pf_interval_merge_consecutive(rel, keys, aggs := 'count(*) AS count', per := []) AS TABLE
FROM query(
  CASE
  WHEN regexp_matches(trim(pf_interval__aggs(aggs)), '^(?i:count\s*\(\s*\*\s*\)\s+as\s+([A-Za-z_][A-Za-z0-9_]*))$') THEN
    'WITH o AS MATERIALIZED (SELECT pf_row_id() AS __pf_r, * FROM ' || pf_interval__src(rel) || ' __pf_t), '
    || 'k AS (SELECT ' || pf_interval__keys(per, '')
    || 'unnest(pf_interval_merge_consecutive_count_agg(__pf_r, ts::BIGINT, dur::BIGINT, (hash('
    || array_to_string(pf_interval__to_list(keys), ', ') || ') & 9223372036854775807)::BIGINT, 1::BIGINT)) AS f FROM o'
    || pf_interval__groupby(per) || ') '
    || 'SELECT f.ts AS ts, f.dur AS dur, ' || pf_interval__keys(per, 'k.')
    || pf_interval__keys(keys, 'o.') || 'f.cnt AS '
    || regexp_extract(trim(pf_interval__aggs(aggs)), '^(?i:count\s*\(\s*\*\s*\)\s+as\s+([A-Za-z_][A-Za-z0-9_]*))$', 1)
    || ' FROM k JOIN o ON o.__pf_r = f.r0'
  ELSE
    'WITH o AS (SELECT *, ts + dur AS __pf_e FROM ' || pf_interval__src(rel) || ' __pf_t), '
    || 'l AS (SELECT *, CASE WHEN lag(__pf_e) OVER __pf_w IS NULL OR ts > lag(__pf_e) OVER __pf_w'
    || array_to_string(list_transform(pf_interval__to_list(keys), c -> ' OR ' || c || ' IS DISTINCT FROM lag(' || c || ') OVER __pf_w'), '')
    || ' THEN 1 ELSE 0 END AS __pf_new FROM o WINDOW __pf_w AS (' || pf_interval__partition(per) || ' ORDER BY ts, dur)), '
    || 'g AS (SELECT *, hash(sum(__pf_new) OVER (' || pf_interval__partition(per)
    || ' ORDER BY ts, dur ROWS UNBOUNDED PRECEDING)) AS __pf_g FROM l) '
    || 'SELECT min(ts) AS ts, max(__pf_e) - min(ts) AS dur, ' || pf_interval__keys(per, '')
    || pf_interval__keys(keys, '') || pf_interval__aggs(aggs) || ' FROM g GROUP BY ' || pf_interval__keys(per, '')
    || pf_interval__keys(keys, '') || '__pf_g'
  END
)
-- @@
-- INTERVAL SET BOUNDS BEGIN AT begin_at (END AT end_at | FOR for_dur):
-- replaces ts / dur (appended as the last two columns).
CREATE OR REPLACE MACRO pf_interval_set_bounds(rel, begin_at, end_at := NULL, for_dur := NULL, per := []::VARCHAR[]) AS TABLE
FROM query(
  'SELECT COLUMNS(c -> c NOT IN (''ts'', ''dur'')), (' || begin_at || ')::BIGINT AS ts, '
  || CASE WHEN end_at IS NOT NULL THEN '((' || end_at || ') - (' || begin_at || '))::BIGINT'
          WHEN for_dur IS NOT NULL THEN '(' || for_dur || ')::BIGINT'
          ELSE error('pf_interval_set_bounds: give end_at or for_dur') END
  || ' AS dur FROM ' || pf_interval__src(rel) || ' __pf_t'
)
-- @@
-- INTERVAL QUANTIZE width ALIGNED TO (TRACE START | ZERO | expr) PER per:
-- rows cut at bucket boundaries origin + k * width; `bucket` = the bucket's
-- start timestamp. aligned_to: 'zero', 'trace_start' (start_ts from the
-- `bounds` relation (per_col, start_ts) if given, else min(ts) of the input
-- per lane) or an SQL expression over the row.
CREATE OR REPLACE MACRO pf_interval_quantize(rel, width, aligned_to := 'zero', bounds := NULL, per := []::VARCHAR[]) AS TABLE
FROM query(
  'WITH o AS (SELECT __pf_t.*, '
  || CASE lower(aligned_to)
       WHEN 'zero' THEN '0::BIGINT'
       WHEN 'trace_start' THEN CASE WHEN bounds IS NULL
         THEN 'min(__pf_t.ts) OVER (' || CASE
           WHEN len(pf_interval__to_list(per)) > 0
           THEN 'PARTITION BY ' || array_to_string([format('__pf_t."{}"', replace(c, '"', '""')) FOR c IN pf_interval__to_list(per)], ', ')
           ELSE '' END || ')'
         ELSE '__pf_b.start_ts' END
       ELSE '(' || aligned_to || ')::BIGINT' END
  || ' AS __pf_o FROM ' || pf_interval__src(rel) || ' __pf_t'
  || CASE WHEN lower(aligned_to) = 'trace_start' AND bounds IS NOT NULL
       THEN ' JOIN ' || pf_interval__src(bounds) || ' __pf_b USING (' || CASE
         WHEN len(pf_interval__to_list(per)) > 0
         THEN format('"{}"', replace(pf_interval__to_list(per)[1], '"', '""'))
         ELSE 'trace_idx' END || ')' ELSE '' END || '), '
  || 'p AS (SELECT *, unnest(pf_interval_quantize_segs(ts::BIGINT, dur::BIGINT, __pf_o, ('
  || width || ')::BIGINT)) AS __pf_q FROM o) '
  || 'SELECT * EXCLUDE (__pf_o, __pf_q) REPLACE (__pf_q.ts AS ts, __pf_q.dur AS dur), '
  || '__pf_q.bucket AS bucket FROM p'
)

-- 5.3 Matching ---------------------------------------------------------------
-- @@
-- [LEFT] INTERVAL JOIN operand AS alias relationship PER per. relationship:
-- 'overlapping', 'covering begin', 'covering end', 'covering bounds',
-- 'within bounds'. Bounds unchanged; one row per match.
CREATE OR REPLACE MACRO pf_interval_join(rel, operand, alias, relationship := 'overlapping',
                                         per := [], left_join := false) AS TABLE
FROM query(
  pf_interval__pair(rel, operand, per, 'pf_interval_join_agg',
                    ', ' || (pf_interval__rel(relationship) + CASE WHEN left_join THEN 256 ELSE 0 END) || '::INTEGER')
  || ' SELECT a.* EXCLUDE (__pf_r, __pf_s), b.__pf_s AS ' || alias
  || ' FROM k JOIN a ON a.__pf_r = f.r0 LEFT JOIN b ON b.__pf_r = f.r1'
)
-- @@
-- [LEFT] INTERVAL FIND operand AS alias (STARTING|ENDING) (AFTER|BEFORE)
-- anchor [WITHIN within] PER per: the single nearest operand row whose start
-- (edge 'starting') or end ('ending') is at/after (or at/before) the anchor
-- ('begin' = ts, 'end' = ts + dur, 'bounds' = end for AFTER, begin for
-- BEFORE). Ties: the first operand row in (key, ts, dur) order in the search
-- direction.
CREATE OR REPLACE MACRO pf_interval_find(rel, operand, alias, edge, direction, anchor := 'begin',
                                         within := NULL, per := [], left_join := false) AS TABLE
FROM query(
  'WITH a AS MATERIALIZED ' || pf_interval__num(rel)
  || ', b AS MATERIALIZED ' || pf_interval__num(operand)
  || ', k AS (SELECT unnest(__pf_a) AS f FROM (SELECT pf_interval_find_agg(side, row, ts, dur, '
  || (
    CASE lower(edge) WHEN 'starting' THEN 0 WHEN 'ending' THEN 1
         ELSE error('pf_interval_find: edge must be starting or ending') END
    + CASE lower(direction) WHEN 'after' THEN 0 WHEN 'before' THEN 2
           ELSE error('pf_interval_find: direction must be after or before') END
    + CASE regexp_extract(lower(trim(anchor)), '^(begin|end|bounds)', 1)
           WHEN 'begin' THEN 0 WHEN 'end' THEN 4 WHEN 'bounds' THEN 8
           ELSE error('pf_interval_find: anchor must be begin, end or bounds') END
    + CASE WHEN left_join THEN 256 ELSE 0 END
  ) || '::INTEGER) AS __pf_a FROM ('
  || CASE
       WHEN regexp_matches(lower(trim(anchor)), '^(begin|end|bounds)\s+of\s+([a-z_][a-z0-9_]*)$') THEN
         'SELECT ' || pf_interval__keys(per, '') || '0::INTEGER AS side, __pf_r AS row, '
         || regexp_extract(trim(anchor), '^(?i:begin|end|bounds)\s+(?i:of)\s+([A-Za-z_][A-Za-z0-9_]*)$', 1) || '.ts::BIGINT AS ts, '
         || regexp_extract(trim(anchor), '^(?i:begin|end|bounds)\s+(?i:of)\s+([A-Za-z_][A-Za-z0-9_]*)$', 1) || '.dur::BIGINT AS dur FROM a WHERE '
         || regexp_extract(trim(anchor), '^(?i:begin|end|bounds)\s+(?i:of)\s+([A-Za-z_][A-Za-z0-9_]*)$', 1) || '.ts IS NOT NULL'
       ELSE pf_interval__feed(per, 0, 'a')
     END
  || ' UNION ALL ' || pf_interval__feed(per, 1, 'b') || ')' || pf_interval__groupby(per) || '))'
  || ' SELECT a.* EXCLUDE (__pf_r, __pf_s), '
  || CASE
       WHEN within IS NULL THEN 'b.__pf_s'
       ELSE 'CASE WHEN ('
            || CASE lower(direction)
                 WHEN 'after' THEN '(' || CASE lower(edge) WHEN 'ending' THEN 'b.ts + b.dur' ELSE 'b.ts' END
                                   || ') - (' || CASE WHEN regexp_extract(lower(trim(anchor)), '^(begin|end|bounds)', 1) = 'begin' THEN
                                        CASE WHEN regexp_matches(lower(trim(anchor)), '\s+of\s+') THEN 'a.' || regexp_extract(trim(anchor), '\s+(?i:of)\s+([A-Za-z_][A-Za-z0-9_]*)$', 1) || '.ts' ELSE 'a.ts' END
                                      ELSE
                                        CASE WHEN regexp_matches(lower(trim(anchor)), '\s+of\s+') THEN 'a.' || regexp_extract(trim(anchor), '\s+(?i:of)\s+([A-Za-z_][A-Za-z0-9_]*)$', 1) || '.ts + a.' || regexp_extract(trim(anchor), '\s+(?i:of)\s+([A-Za-z_][A-Za-z0-9_]*)$', 1) || '.dur' ELSE 'a.ts + a.dur' END
                                      END || ')'
                 ELSE '(' || CASE WHEN regexp_extract(lower(trim(anchor)), '^(begin|end|bounds)', 1) IN ('begin', 'bounds') THEN
                               CASE WHEN regexp_matches(lower(trim(anchor)), '\s+of\s+') THEN 'a.' || regexp_extract(trim(anchor), '\s+(?i:of)\s+([A-Za-z_][A-Za-z0-9_]*)$', 1) || '.ts' ELSE 'a.ts' END
                             ELSE
                               CASE WHEN regexp_matches(lower(trim(anchor)), '\s+of\s+') THEN 'a.' || regexp_extract(trim(anchor), '\s+(?i:of)\s+([A-Za-z_][A-Za-z0-9_]*)$', 1) || '.ts + a.' || regexp_extract(trim(anchor), '\s+(?i:of)\s+([A-Za-z_][A-Za-z0-9_]*)$', 1) || '.dur' ELSE 'a.ts + a.dur' END
                             END
                      || ') - (' || CASE lower(edge) WHEN 'ending' THEN 'b.ts + b.dur' ELSE 'b.ts' END || ')'
               END
            || ') <= (' || within || ') THEN b.__pf_s END'
     END
  || ' AS ' || alias
  || CASE
       WHEN left_join AND regexp_matches(lower(trim(anchor)), '\s+of\s+') THEN
         ' FROM a LEFT JOIN k ON a.__pf_r = k.f.r0 LEFT JOIN b ON b.__pf_r = k.f.r1'
       ELSE
         ' FROM k JOIN a ON a.__pf_r = f.r0 LEFT JOIN b ON b.__pf_r = f.r1'
     END
  || CASE
       WHEN within IS NULL OR left_join THEN ''
       ELSE ' WHERE ('
            || CASE lower(direction)
                 WHEN 'after' THEN '(' || CASE lower(edge) WHEN 'ending' THEN 'b.ts + b.dur' ELSE 'b.ts' END
                                   || ') - (' || CASE WHEN regexp_extract(lower(trim(anchor)), '^(begin|end|bounds)', 1) = 'begin' THEN
                                        CASE WHEN regexp_matches(lower(trim(anchor)), '\s+of\s+') THEN 'a.' || regexp_extract(trim(anchor), '\s+(?i:of)\s+([A-Za-z_][A-Za-z0-9_]*)$', 1) || '.ts' ELSE 'a.ts' END
                                      ELSE
                                        CASE WHEN regexp_matches(lower(trim(anchor)), '\s+of\s+') THEN 'a.' || regexp_extract(trim(anchor), '\s+(?i:of)\s+([A-Za-z_][A-Za-z0-9_]*)$', 1) || '.ts + a.' || regexp_extract(trim(anchor), '\s+(?i:of)\s+([A-Za-z_][A-Za-z0-9_]*)$', 1) || '.dur' ELSE 'a.ts + a.dur' END
                                      END || ')'
                 ELSE '(' || CASE WHEN regexp_extract(lower(trim(anchor)), '^(begin|end|bounds)', 1) IN ('begin', 'bounds') THEN
                               CASE WHEN regexp_matches(lower(trim(anchor)), '\s+of\s+') THEN 'a.' || regexp_extract(trim(anchor), '\s+(?i:of)\s+([A-Za-z_][A-Za-z0-9_]*)$', 1) || '.ts' ELSE 'a.ts' END
                             ELSE
                               CASE WHEN regexp_matches(lower(trim(anchor)), '\s+of\s+') THEN 'a.' || regexp_extract(trim(anchor), '\s+(?i:of)\s+([A-Za-z_][A-Za-z0-9_]*)$', 1) || '.ts + a.' || regexp_extract(trim(anchor), '\s+(?i:of)\s+([A-Za-z_][A-Za-z0-9_]*)$', 1) || '.dur' ELSE 'a.ts + a.dur' END
                             END
                      || ') - (' || CASE lower(edge) WHEN 'ending' THEN 'b.ts + b.dur' ELSE 'b.ts' END || ')'
               END
            || ') <= (' || within || ')'
     END
)
-- @@
-- INTERVAL KEEP IF relationship operand PER per: rows standing in the
-- relationship to the operand's merged coverage.
CREATE OR REPLACE MACRO pf_interval_keep_if(rel, operand, relationship := 'overlapping', per := []) AS TABLE
FROM query(
  pf_interval__pair(rel, operand, per, 'pf_interval_keep_agg',
                    ', ' || pf_interval__rel(relationship) || '::INTEGER')
  || ' SELECT * EXCLUDE (__pf_r, __pf_s) FROM a WHERE __pf_r IN (SELECT f.r0 FROM k)'
)
-- @@
-- INTERVAL DROP IF relationship operand PER per: the complement of KEEP IF.
CREATE OR REPLACE MACRO pf_interval_drop_if(rel, operand, relationship := 'overlapping', per := []) AS TABLE
FROM query(
  pf_interval__pair(rel, operand, per, 'pf_interval_keep_agg',
                    ', ' || pf_interval__rel(relationship) || '::INTEGER')
  || ' SELECT * EXCLUDE (__pf_r, __pf_s) FROM a WHERE __pf_r NOT IN (SELECT f.r0 FROM k)'
)
