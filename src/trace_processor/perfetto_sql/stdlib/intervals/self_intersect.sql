--
-- Copyright 2024 The Android Open Source Project
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

CREATE PERFETTO MACRO _ii_si_agg(x ColumnName, y ColumnName)
RETURNS _ProjectionFragment AS __intrinsic_stringify!($x), input.$y;

CREATE PERFETTO MACRO _ii_si_bind(x Expr, y Expr)
RETURNS Expr AS __intrinsic_table_ptr_bind($x, __intrinsic_stringify!($y));

CREATE PERFETTO MACRO _ii_si_select(x ColumnName, y Expr)
RETURNS _ProjectionFragment AS $x AS $y;

CREATE PERFETTO MACRO _ii_si_id(x ColumnName)
RETURNS _ProjectionFragment AS $x;

-- Creates an aggregation specification for use with interval_intersect!.
-- Similar to tree_agg!, this returns a typed INTERVAL_AGG pointer.
--
-- Supported aggregation types:
-- - COUNT: Count of overlapping intervals (column name is ignored, use any name)
-- - SUM: Sum of values from the specified column
-- - MIN: Minimum value from the specified column
-- - MAX: Maximum value from the specified column
-- - AVG: Average value from the specified column
--
-- The first parameter is the SOURCE column name from the input table.
-- The aggregation result will be named as <agg_type>_<col> (e.g., sum_value, max_priority).
--
-- Example:
--   interval_agg!(value, SUM)    -- aggregates the 'value' column, output as 'sum_value'
--   interval_agg!(priority, MAX) -- aggregates the 'priority' column, output as 'max_priority'
--   interval_agg!(count, COUNT)  -- counts overlaps, output as 'count'
CREATE PERFETTO MACRO interval_agg(
  -- The source column name to aggregate from the input table
  col ColumnName,
  -- The aggregation type (COUNT, SUM, MIN, MAX, AVG)
  agg_type ColumnName
)
RETURNS Expr AS
__intrinsic_interval_agg(__intrinsic_stringify!($col), __intrinsic_stringify!($agg_type));

-- Creates a partitioned interval set from a table.
CREATE PERFETTO MACRO interval_partition(
  tab TableOrSubquery,
  partition_cols ColumnNameList,
  agg_cols ColumnNameList
)
RETURNS Expr AS
(
  SELECT
    __intrinsic_interval_tree_intervals_agg(
      input.id,
      input.ts,
      input.dur
      __intrinsic_token_apply_prefix!(
        _ii_si_agg,
        $agg_cols,
        $agg_cols
      ),
      '__PERFETTO_PARTITION_DELIMITER__'
      __intrinsic_token_apply_prefix!(
        _ii_si_agg,
        $partition_cols,
        $partition_cols
      )
    )
  FROM (SELECT * FROM $tab ORDER BY ts) input
);

-- Creates a partitioned interval set from a table, with columns for aggregation.
-- Currently supports exactly 1 aggregation column and at most 1 partition column.
--
-- Example:
--   interval_partition_with_agg!(my_table, (cpu), (value))
--   interval_partition_with_agg!(my_table, (), (value))
CREATE PERFETTO MACRO interval_partition_with_agg(
  tab TableOrSubquery,
  partition_cols ColumnNameList,
  agg_cols ColumnNameList
)
RETURNS Expr AS interval_partition!($tab, $partition_cols, $agg_cols);

-- Helper macro to pass aggregations to the intrinsic function
CREATE PERFETTO MACRO _ii_si_pass_agg(x Expr)
RETURNS Expr AS $x;

-- Computes the self-intersection of the partitioned intervals.
-- Accepts up to 8 interval_agg! specifications in a parenthesized list.
--
-- Example with 1 aggregation:
--   interval_intersect!(partitions, (interval_agg!(count, COUNT)))
--
-- Example with multiple aggregations:
--   interval_intersect!(partitions, (
--     interval_agg!(count, COUNT),
--     interval_agg!(sum, SUM),
--     interval_agg!(max, MAX)
--   ))
CREATE PERFETTO MACRO interval_intersect(
  partitions Expr,
  aggs ColumnNameList
)
RETURNS Expr AS
__intrinsic_interval_self_intersect(
  $partitions
  __intrinsic_token_apply_prefix!(
    _ii_si_pass_agg,
    $aggs
  )
);

-- Converts the partitioned intervals back to a table.
CREATE PERFETTO MACRO interval_to_table(
  partitions Expr,
  columns ColumnNameList
)
RETURNS TableOrSubquery AS
(
  SELECT
    __intrinsic_token_apply!(_ii_si_id, $columns)
  FROM (
    SELECT
      0 AS _dummy
      __intrinsic_token_apply_prefix!(
        _ii_si_select,
        (c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13, c14, c15),
        $columns
      )
    FROM __intrinsic_table_ptr(__intrinsic_interval_to_table($partitions))
    WHERE TRUE
      __intrinsic_token_apply_and_prefix!(
        _ii_si_bind,
        (c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13, c14, c15),
        $columns
      )
  )
);


