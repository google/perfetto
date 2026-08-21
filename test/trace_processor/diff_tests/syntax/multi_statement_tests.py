#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
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


class PerfettoMultiStatement(TestSuite):
  # Each statement's result set is printed, separated by a blank line.

  def test_multiple_selects(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT 1 AS a, 2 AS b;
        SELECT 'foo' AS c;
        """,
        out=Csv("""
        "a","b"
        1,2

        "c"
        "foo"
        """))

  def test_no_output_statements_between_selects(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        CREATE PERFETTO TABLE foo AS SELECT 42 AS x;
        SELECT x FROM foo;
        CREATE PERFETTO TABLE bar AS SELECT 43 AS y;
        SELECT y FROM bar;
        """,
        out=Csv("""
        "x"
        42

        "y"
        43
        """))

  def test_suppress_query_output_not_printed(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT 1 AS suppress_query_output;
        SELECT 2 AS a;
        """,
        out=Csv("""
        "a"
        2
        """))

  def test_zero_row_result_set_prints_header(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT 1 AS a WHERE 0;
        SELECT 2 AS b;
        """,
        out=Csv("""
        "a"

        "b"
        2
        """))

  def test_three_selects(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT 1 AS a;
        SELECT 2 AS b;
        SELECT 3 AS c;
        """,
        out=Csv("""
        "a"
        1

        "b"
        2

        "c"
        3
        """))
