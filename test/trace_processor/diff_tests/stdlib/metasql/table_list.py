#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License a
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class TableListTests(TestSuite):

  def test_table_list_result_columns(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        INCLUDE PERFETTO MODULE metasql.table_list;

        CREATE PERFETTO MACRO mac(t TableOrSubquery)
        RETURNS TableOrSubquery AS
        (SELECT * FROM $t);

        WITH foo AS (
          SELECT 0 AS a
        ),
        bar AS (
          SELECT 1 AS b
        ),
        baz AS (
          SELECT 2 AS c
        )
        SELECT a + b + c
        FROM _metasql_map_join_table_list!((foo, bar, baz), mac);
        """,
        out=Csv("""
        "a + b + c"
        3
        """))

  def test_table_list_with_capture(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        INCLUDE PERFETTO MODULE metasql.table_list;

        CREATE PERFETTO MACRO mac(t TableOrSubquery, x Expr)
        RETURNS TableOrSubquery AS
        (SELECT *, $x AS bla FROM $t);

        WITH foo AS (
          SELECT 0 AS a
        ),
        bar AS (
          SELECT 1 AS b
        ),
        baz AS (
          SELECT 2 AS c
        )
        SELECT
          a + b + c
        FROM _metasql_map_join_table_list_with_capture!(
          (foo, bar, baz),
          mac,
          (3)
        );
        """,
        out=Csv("""
        "a + b + c"
        3
        """))
