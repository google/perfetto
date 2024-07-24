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

CREATE PERFETTO MACRO _col_list_id(a ColumnName)
RETURNS _SqlFragment AS $a;

-- Given a list of column names, applies an arbitrary macro to each column
-- and joins the result with a comma.
CREATE PERFETTO MACRO _metasql_map_join_column_list(
  columns _ColumnNameList,
  map_macro _Macro
)
RETURNS _SqlFragment
AS __intrinsic_token_map_join!($columns, $map_macro, __intrinsic_token_comma!());

-- Given a list of column names, removes the parentheses allowing the usage
-- of these in a select statement, window function etc.
CREATE PERFETTO MACRO _metasql_unparenthesize_column_list(
  columns _ColumnNameList
)
RETURNS _SqlFragment
AS _metasql_map_join_column_list!($columns, _col_list_id);
