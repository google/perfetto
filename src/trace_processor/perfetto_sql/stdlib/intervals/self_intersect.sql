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

-- Simple version - just returns count of overlapping intervals
CREATE PERFETTO MACRO _interval_self_intersect(
  tab TableOrSubquery,
  partition_cols ColumnNameList
)
RETURNS TableOrSubquery AS
(
  SELECT
    c0 AS ts,
    c1 AS dur,
    c2 AS group_id,
    c3 AS count
    __intrinsic_token_apply_prefix!(
      _ii_si_select,
      (c4, c5, c6, c7),
      $partition_cols
    )
  FROM __intrinsic_table_ptr(
    __intrinsic_interval_self_intersect(
      (
        SELECT
          __intrinsic_interval_tree_intervals_agg(
            input.id,
            input.ts,
            input.dur
            __intrinsic_token_apply_prefix!(
              _ii_si_agg,
              $partition_cols,
              $partition_cols
            )
          )
        FROM (SELECT * FROM $tab ORDER BY ts) input
      ),
      __intrinsic_stringify!($partition_cols),
      'count'
    )
  )
  WHERE __intrinsic_table_ptr_bind(c0, 'ts')
    AND __intrinsic_table_ptr_bind(c1, 'dur')
    AND __intrinsic_table_ptr_bind(c2, 'group_id')
    AND __intrinsic_table_ptr_bind(c3, 'count')
    __intrinsic_token_apply_and_prefix!(
      _ii_si_bind,
      (c4, c5, c6, c7),
      $partition_cols
    )
);

-- With sum aggregation on a column
CREATE PERFETTO MACRO _interval_self_intersect_sum(
  tab TableOrSubquery,
  partition_cols ColumnNameList,
  agg_col ColumnName
)
RETURNS TableOrSubquery AS
(
  SELECT
    c0 AS ts,
    c1 AS dur,
    c2 AS group_id,
    c3 AS count,
    c4 AS sum
    __intrinsic_token_apply_prefix!(
      _ii_si_select,
      (c5, c6, c7, c8),
      $partition_cols
    )
  FROM __intrinsic_table_ptr(
    __intrinsic_interval_self_intersect(
      (
        SELECT
          __intrinsic_interval_tree_intervals_agg(
            input.id,
            input.ts,
            input.dur,
            input.$agg_col
            __intrinsic_token_apply_prefix!(
              _ii_si_agg,
              $partition_cols,
              $partition_cols
            )
          )
        FROM (SELECT * FROM $tab ORDER BY ts) input
      ),
      __intrinsic_stringify!($partition_cols),
      'count,sum:' || __intrinsic_stringify!($agg_col)
    )
  )
  WHERE __intrinsic_table_ptr_bind(c0, 'ts')
    AND __intrinsic_table_ptr_bind(c1, 'dur')
    AND __intrinsic_table_ptr_bind(c2, 'group_id')
    AND __intrinsic_table_ptr_bind(c3, 'count')
    AND __intrinsic_table_ptr_bind(c4, 'sum_' || __intrinsic_stringify!($agg_col))
    __intrinsic_token_apply_and_prefix!(
      _ii_si_bind,
      (c5, c6, c7, c8),
      $partition_cols
    )
);

-- With max aggregation on a column
CREATE PERFETTO MACRO _interval_self_intersect_max(
  tab TableOrSubquery,
  partition_cols ColumnNameList,
  agg_col ColumnName
)
RETURNS TableOrSubquery AS
(
  SELECT
    c0 AS ts,
    c1 AS dur,
    c2 AS group_id,
    c3 AS count,
    c4 AS max
    __intrinsic_token_apply_prefix!(
      _ii_si_select,
      (c5, c6, c7, c8),
      $partition_cols
    )
  FROM __intrinsic_table_ptr(
    __intrinsic_interval_self_intersect(
      (
        SELECT
          __intrinsic_interval_tree_intervals_agg(
            input.id,
            input.ts,
            input.dur,
            input.$agg_col
            __intrinsic_token_apply_prefix!(
              _ii_si_agg,
              $partition_cols,
              $partition_cols
            )
          )
        FROM (SELECT * FROM $tab ORDER BY ts) input
      ),
      __intrinsic_stringify!($partition_cols),
      'count,max:' || __intrinsic_stringify!($agg_col)
    )
  )
  WHERE __intrinsic_table_ptr_bind(c0, 'ts')
    AND __intrinsic_table_ptr_bind(c1, 'dur')
    AND __intrinsic_table_ptr_bind(c2, 'group_id')
    AND __intrinsic_table_ptr_bind(c3, 'count')
    AND __intrinsic_table_ptr_bind(c4, 'max_' || __intrinsic_stringify!($agg_col))
    __intrinsic_token_apply_and_prefix!(
      _ii_si_bind,
      (c5, c6, c7, c8),
      $partition_cols
    )
);

-- With min aggregation on a column
CREATE PERFETTO MACRO _interval_self_intersect_min(
  tab TableOrSubquery,
  partition_cols ColumnNameList,
  agg_col ColumnName
)
RETURNS TableOrSubquery AS
(
  SELECT
    c0 AS ts,
    c1 AS dur,
    c2 AS group_id,
    c3 AS count,
    c4 AS min
    __intrinsic_token_apply_prefix!(
      _ii_si_select,
      (c5, c6, c7, c8),
      $partition_cols
    )
  FROM __intrinsic_table_ptr(
    __intrinsic_interval_self_intersect(
      (
        SELECT
          __intrinsic_interval_tree_intervals_agg(
            input.id,
            input.ts,
            input.dur,
            input.$agg_col
            __intrinsic_token_apply_prefix!(
              _ii_si_agg,
              $partition_cols,
              $partition_cols
            )
          )
        FROM (SELECT * FROM $tab ORDER BY ts) input
      ),
      __intrinsic_stringify!($partition_cols),
      'count,min:' || __intrinsic_stringify!($agg_col)
    )
  )
  WHERE __intrinsic_table_ptr_bind(c0, 'ts')
    AND __intrinsic_table_ptr_bind(c1, 'dur')
    AND __intrinsic_table_ptr_bind(c2, 'group_id')
    AND __intrinsic_table_ptr_bind(c3, 'count')
    AND __intrinsic_table_ptr_bind(c4, 'min_' || __intrinsic_stringify!($agg_col))
    __intrinsic_token_apply_and_prefix!(
      _ii_si_bind,
      (c5, c6, c7, c8),
      $partition_cols
    )
);

-- With avg aggregation on a column
CREATE PERFETTO MACRO _interval_self_intersect_avg(
  tab TableOrSubquery,
  partition_cols ColumnNameList,
  agg_col ColumnName
)
RETURNS TableOrSubquery AS
(
  SELECT
    c0 AS ts,
    c1 AS dur,
    c2 AS group_id,
    c3 AS count,
    c4 AS avg
    __intrinsic_token_apply_prefix!(
      _ii_si_select,
      (c5, c6, c7, c8),
      $partition_cols
    )
  FROM __intrinsic_table_ptr(
    __intrinsic_interval_self_intersect(
      (
        SELECT
          __intrinsic_interval_tree_intervals_agg(
            input.id,
            input.ts,
            input.dur,
            input.$agg_col
            __intrinsic_token_apply_prefix!(
              _ii_si_agg,
              $partition_cols,
              $partition_cols
            )
          )
        FROM (SELECT * FROM $tab ORDER BY ts) input
      ),
      __intrinsic_stringify!($partition_cols),
      'count,avg:' || __intrinsic_stringify!($agg_col)
    )
  )
  WHERE __intrinsic_table_ptr_bind(c0, 'ts')
    AND __intrinsic_table_ptr_bind(c1, 'dur')
    AND __intrinsic_table_ptr_bind(c2, 'group_id')
    AND __intrinsic_table_ptr_bind(c3, 'count')
    AND __intrinsic_table_ptr_bind(c4, 'avg_' || __intrinsic_stringify!($agg_col))
    __intrinsic_token_apply_and_prefix!(
      _ii_si_bind,
      (c5, c6, c7, c8),
      $partition_cols
    )
);
