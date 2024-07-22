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
RETURNS Expr AS __intrinsic_stringify!($x), $y;

CREATE PERFETTO MACRO _ii_df_bind(x Expr, y Expr)
RETURNS Expr AS __intrinsic_table_ptr_bind($x, __intrinsic_stringify!($y));

CREATE PERFETTO MACRO _ii_df_select(x Expr, y Expr)
RETURNS Expr AS $x AS $y;

CREATE PERFETTO MACRO _interval_agg(
  tab TableOrSubquery,
  agg_columns _ColumnNameList
)
RETURNS TableOrSubquery AS
(
  SELECT
    __intrinsic_interval_tree_intervals_agg(
      id,
      ts,
      dur
      __intrinsic_prefixed_token_zip_join!(
        $agg_columns,
        $agg_columns,
        _ii_df_agg,
        __intrinsic_token_comma!()
      )
    )
  FROM $tab
  ORDER BY ts
);

CREATE PERFETTO MACRO _interval_intersect(
  t1 TableOrSubquery,
  t2 TableOrSubquery,
  agg_columns _ColumnNameList
)
RETURNS TableOrSubquery AS
(
  SELECT
    c0 AS ts,
    c1 AS dur,
    c2 AS id_0,
    c3 AS id_1
    __intrinsic_prefixed_token_zip_join!(
      (c4, c5, c6, c7, c8, c9, c10),
      $agg_columns,
      _ii_df_select,
      __intrinsic_token_comma!()
    )
  FROM __intrinsic_table_ptr(
    __intrinsic_interval_intersect(
      _interval_agg!($t1, $agg_columns),
      _interval_agg!($t2, $agg_columns),
      __intrinsic_stringify!($agg_columns)
    )
  )
  WHERE __intrinsic_table_ptr_bind(c0, 'ts')
    AND __intrinsic_table_ptr_bind(c1, 'dur')
    AND __intrinsic_table_ptr_bind(c2, 'id_0')
    AND __intrinsic_table_ptr_bind(c3, 'id_1')
    __intrinsic_prefixed_token_zip_join!(
        (c4, c5, c6, c7, c8, c9, c10),
        $agg_columns,
        _ii_df_bind,
        AND
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
    $t,
    (SELECT 0 AS id, $ts AS ts, $dur AS dur),
    ()
  )
);
