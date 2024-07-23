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

-- Given a list of table names, applies an arbitrary macro to each table
-- and joins the result with a comma.
CREATE PERFETTO MACRO _metasql_map_join_table_list(
  tables _TableNameList,
  map_macro _Macro
)
RETURNS _SqlFragment
AS __intrinsic_token_map_join!($tables, $map_macro, __intrinsic_token_comma!());

-- Given a list of table names, applies an arbitrary macro to each table
-- and joins the result with a comma.
CREATE PERFETTO MACRO _metasql_map_join_table_list_with_capture(
  tables _TableNameList,
  map_macro _Macro,
  args _ArgumentList
)
RETURNS _SqlFragment
AS __intrinsic_token_map_join_with_capture!($tables, $map_macro, $args, __intrinsic_token_comma!());
