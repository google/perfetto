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

CREATE PERFETTO MACRO _ii_df_agg(x Expr, y Expr)
RETURNS Expr AS __intrinsic_stringify!($x), input.$y;

CREATE PERFETTO MACRO _ii_df_bind(x Expr, y Expr)
RETURNS Expr AS __intrinsic_table_ptr_bind($x, __intrinsic_stringify!($y));

CREATE PERFETTO MACRO _ii_df_select(x Expr, y Expr)
RETURNS Expr AS $x AS $y;

CREATE PERFETTO MACRO __first_arg(x Expr, y Expr)
RETURNS Expr AS $x;

CREATE PERFETTO MACRO _interval_agg(
  tab TableOrSubquery,
  agg_columns _ColumnNameList
)
RETURNS TableOrSubquery AS
(
  SELECT __intrinsic_interval_tree_intervals_agg(
    input.id,
    input.ts,
    input.dur
    __intrinsic_token_apply_prefix!(
      _ii_df_agg,
      $agg_columns,
      $agg_columns
    )
  )
  FROM (SELECT * FROM $tab ORDER BY ts) input
);

CREATE PERFETTO MACRO _interval_intersect(
  tabs _TableNameList,
  agg_columns _ColumnNameList
)
RETURNS TableOrSubquery AS
(
  SELECT
    c0 AS ts,
    c1 AS dur,
    -- Columns for tables ids, in the order of provided tables.
    __intrinsic_token_apply!(
      __first_arg,
      (c2 AS id_0, c3 AS id_1, c4 AS id_2, c5 AS id_3, c6 AS id_4),
      $tabs
    )
    -- Columns for partitions, one for each column with partition.
    __intrinsic_token_apply_prefix!(
      _ii_df_select,
      (c7, c8, c9, c10),
      $agg_columns
    )
  -- Interval intersect result table.
  FROM __intrinsic_table_ptr(
    __intrinsic_interval_intersect(
      __intrinsic_token_apply!(
        _interval_agg,
        $tabs,
        ($agg_columns, $agg_columns, $agg_columns, $agg_columns, $agg_columns)
      ),
      __intrinsic_stringify!($agg_columns)
    )
  )

  -- Bind the resulting columns
  WHERE __intrinsic_table_ptr_bind(c0, 'ts')
    AND __intrinsic_table_ptr_bind(c1, 'dur')
    -- Id columns
    AND __intrinsic_table_ptr_bind(c2, 'id_0')
    AND __intrinsic_table_ptr_bind(c3, 'id_1')
    AND __intrinsic_table_ptr_bind(c4, 'id_2')
    AND __intrinsic_table_ptr_bind(c5, 'id_3')
    AND __intrinsic_table_ptr_bind(c6, 'id_4')

    -- Partition columns.
    __intrinsic_token_apply_and_prefix!(
      _ii_df_bind,
      (c7, c8, c9, c10),
      $agg_columns
    )
);

CREATE PERFETTO MACRO _interval_intersect_single(
  ts Expr,
  dur Expr,
  t TableOrSubquery
)
RETURNS TableOrSubquery AS
(
  SELECT
  id_0 AS id,
  ts,
  dur
  FROM _interval_intersect!(
    ($t, (SELECT 0 AS id, $ts AS ts, $dur AS dur)),
    ()
  )
);
