#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from python.generators.diff_tests.testing import PrintProfileProto
from google.protobuf import text_format


class PerfettoTableFunction(TestSuite):

  def test_create_table_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO FUNCTION f(x INT) RETURNS TABLE(y INT) AS SELECT $x + 1 as y;

        SELECT * FROM f(5);
      """,
        out=Csv("""
        "y"
        6
      """))

  def test_replace_table_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO FUNCTION f(x INT) RETURNS TABLE(y INT) AS SELECT $x + 1 as y;
        CREATE OR REPLACE PERFETTO FUNCTION f(x INT) RETURNS TABLE(y INT) AS SELECT $x + 2 as y;

        SELECT * FROM f(5);
      """,
        out=Csv("""
        "y"
        7
      """))

  def test_legacy_create_view_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT create_view_function('f(x INT)', 'result INT', 'SELECT $x + 1 as result');

        SELECT * FROM f(5);
      """,
        out=Csv("""
        "result"
        6
      """))

  def test_legacy_table_function_drop_partial(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
          CREATE TABLE bar AS SELECT 1;

          CREATE OR REPLACE PERFETTO FUNCTION foo()
          RETURNS TABLE(x INT) AS
          SELECT 1 AS x
          UNION
          SELECT * FROM bar;

          CREATE TABLE res AS SELECT * FROM foo() LIMIT 1;

          DROP TABLE bar;
        """,
        out=Csv(""))
