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

-- sqlformat file off

-- Self-intersection of an interval table. Identical output contract to the
-- existing intervals.intersect.interval_self_intersect SQL macro — drop-in
-- replacement, implemented via the C++ plugin
-- plugins/interval_self_intersect (single-pass O(n log n) sweep).
--
-- For each atomic time segment defined by the endpoints of |intervals| and
-- for every interval active in that segment, emits one row with the
-- interval's id and interval_ends_at_ts = 0. For each interval, also emits
-- one "end marker" row at the segment that begins at the interval's end ts
-- with interval_ends_at_ts = 1; the final endpoint produces a dur=0
-- segment containing only end markers (matching the SQL macro's quirk).
--
-- |intervals| must expose `id INT64`, `ts INT64`, `dur INT64`.
-- Output:
--   ts INT64                start of the atomic segment
--   dur INT64               duration to the next endpoint (0 at the final)
--   group_id INT64          1-indexed stable per-segment id
--   id INT64                original interval id
--   interval_ends_at_ts INT64    0 = active in segment, 1 = end marker
CREATE PERFETTO MACRO _interval_self_intersect(intervals TableOrSubquery)
RETURNS TableOrSubquery
AS (
  SELECT
    c0 AS ts,
    c1 AS dur,
    c2 AS group_id,
    c3 AS id,
    c4 AS interval_ends_at_ts
  FROM __intrinsic_table_ptr(
    __intrinsic_interval_self_intersect(
      (
        SELECT
          __intrinsic_interval_tree_intervals_agg(
            input.id, input.ts, input.dur
          )
        FROM (SELECT * FROM $intervals ORDER BY ts) input
      )
    )
  )
  WHERE __intrinsic_table_ptr_bind(c0, 'ts')
    AND __intrinsic_table_ptr_bind(c1, 'dur')
    AND __intrinsic_table_ptr_bind(c2, 'group_id')
    AND __intrinsic_table_ptr_bind(c3, 'id')
    AND __intrinsic_table_ptr_bind(c4, 'interval_ends_at_ts')
);

-- Helper macros for the aggregated variant below.
CREATE PERFETTO MACRO _isi_agg_pair(x ColumnName, y ColumnName)
RETURNS _ProjectionFragment
AS __intrinsic_stringify!($x), input.$y;

CREATE PERFETTO MACRO _isi_select(x ColumnName, y Expr)
RETURNS _ProjectionFragment
AS $x AS $y;

CREATE PERFETTO MACRO _isi_bind(x Expr, y Expr)
RETURNS Expr
AS __intrinsic_table_ptr_bind($x, __intrinsic_stringify!($y));

-- Partitioned self-intersect with aggregation done during the sweep.
--
-- Splits the timeline of each partition (tuple of $partition_cols values,
-- NULL keys form their own partition) into atomic segments at every interval
-- endpoint and emits ONE row per run of adjacent segments with identical
-- aggregates — a boundary where as many intervals start as end (e.g.
-- back-to-back slices with equal values) does not emit a row. Zero-active
-- gap segments are emitted too, as is a trailing segment ending at the
-- partition's last endpoint, so a counter sourced from this output drops to
-- zero where cover ends. Output size is at most 2x the input rows (per
-- partition), regardless of overlap depth.
--
-- |intervals| must expose `ts INT64`, `dur INT64` (>= 0), $value_col
-- (numeric or NULL), and the $partition_cols. Input need not be sorted.
--
-- Output:
--   ts INT64          start of the (merged) segment
--   dur INT64         duration until the aggregates next change in the
--                     partition (0 at the trailing segment)
--   group_id INT64    globally unique 1-indexed segment id
--   cnt INT64         number of intervals active in the segment
--   sum_value DOUBLE  sum of $value_col over active intervals (0 when none)
--   min_value DOUBLE  min of $value_col over active intervals (NULL when none)
--   max_value DOUBLE  max of $value_col over active intervals (NULL when none)
--   <partition cols>  the partition tuple, with their original names
CREATE PERFETTO MACRO _interval_self_intersect_agg(
  intervals TableOrSubquery,
  value_col ColumnName,
  partition_cols ColumnNameList
)
RETURNS TableOrSubquery
AS (
  SELECT
    c0 AS ts,
    c1 AS dur,
    c2 AS group_id,
    c3 AS cnt,
    c4 AS sum_value,
    c5 AS min_value,
    c6 AS max_value
    __intrinsic_token_apply_prefix!(
      _isi_select,
      (c7, c8, c9, c10, c11, c12, c13, c14, c15, c16, c17, c18, c19, c20, c21),
      $partition_cols
    )
  FROM __intrinsic_table_ptr(
    __intrinsic_interval_self_intersect_agg(
      (
        SELECT __intrinsic_isi_intervals_agg(
          input.ts, input.dur, input.$value_col
          __intrinsic_token_apply_prefix!(
            _isi_agg_pair,
            $partition_cols,
            $partition_cols
          )
        )
        FROM $intervals input
      ),
      __intrinsic_stringify!($partition_cols)
    )
  )
  WHERE __intrinsic_table_ptr_bind(c0, 'ts')
    AND __intrinsic_table_ptr_bind(c1, 'dur')
    AND __intrinsic_table_ptr_bind(c2, 'group_id')
    AND __intrinsic_table_ptr_bind(c3, 'cnt')
    AND __intrinsic_table_ptr_bind(c4, 'sum_value')
    AND __intrinsic_table_ptr_bind(c5, 'min_value')
    AND __intrinsic_table_ptr_bind(c6, 'max_value')
    __intrinsic_token_apply_and_prefix!(
      _isi_bind,
      (c7, c8, c9, c10, c11, c12, c13, c14, c15, c16, c17, c18, c19, c20, c21),
      $partition_cols
    )
);

-- Count-only variant of _interval_self_intersect_agg for callers with no
-- value column: sum_value/min_value/max_value are emitted but carry no
-- information (0/NULL/NULL). Same output contract otherwise.
CREATE PERFETTO MACRO _interval_self_intersect_count(
  intervals TableOrSubquery,
  partition_cols ColumnNameList
)
RETURNS TableOrSubquery
AS (
  SELECT
    c0 AS ts,
    c1 AS dur,
    c2 AS group_id,
    c3 AS cnt
    __intrinsic_token_apply_prefix!(
      _isi_select,
      (c7, c8, c9, c10, c11, c12, c13, c14, c15, c16, c17, c18, c19, c20, c21),
      $partition_cols
    )
  FROM __intrinsic_table_ptr(
    __intrinsic_interval_self_intersect_agg(
      (
        SELECT __intrinsic_isi_intervals_agg(
          input.ts, input.dur, NULL
          __intrinsic_token_apply_prefix!(
            _isi_agg_pair,
            $partition_cols,
            $partition_cols
          )
        )
        FROM $intervals input
      ),
      __intrinsic_stringify!($partition_cols)
    )
  )
  WHERE __intrinsic_table_ptr_bind(c0, 'ts')
    AND __intrinsic_table_ptr_bind(c1, 'dur')
    AND __intrinsic_table_ptr_bind(c2, 'group_id')
    AND __intrinsic_table_ptr_bind(c3, 'cnt')
    AND __intrinsic_table_ptr_bind(c4, 'sum_value')
    AND __intrinsic_table_ptr_bind(c5, 'min_value')
    AND __intrinsic_table_ptr_bind(c6, 'max_value')
    __intrinsic_token_apply_and_prefix!(
      _isi_bind,
      (c7, c8, c9, c10, c11, c12, c13, c14, c15, c16, c17, c18, c19, c20, c21),
      $partition_cols
    )
);
