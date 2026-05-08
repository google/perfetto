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


class StdlibMetasql(TestSuite):

  def test_unparenthesize_exprlist(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        INCLUDE PERFETTO MODULE std.metasql.unparenthesize;

        CREATE PERFETTO TABLE data AS
        SELECT 1 AS a, 10 AS b, 100 AS c
        UNION ALL SELECT 1, 20, 200
        UNION ALL SELECT 2, 30, 300
        UNION ALL SELECT 2, 40, 400;

        SELECT
          a,
          SUM(b) AS sum_b,
          SUM(c) AS sum_c
        FROM data
        GROUP BY metasql_unparenthesize_exprlist!((a))
        ORDER BY a;
        """,
        out=Csv("""
        "a","sum_b","sum_c"
        1,30,300
        2,70,700
        """))

  def test_unparenthesize_exprlist_multiple(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        INCLUDE PERFETTO MODULE std.metasql.unparenthesize;

        CREATE PERFETTO TABLE data AS
        SELECT 1 AS a, 'x' AS b, 100 AS c
        UNION ALL SELECT 1, 'x', 200
        UNION ALL SELECT 1, 'y', 300
        UNION ALL SELECT 2, 'x', 400;

        SELECT
          a,
          b,
          SUM(c) AS sum_c
        FROM data
        GROUP BY metasql_unparenthesize_exprlist!((a, b))
        ORDER BY a, b;
        """,
        out=Csv("""
        "a","b","sum_c"
        1,"x",300
        1,"y",300
        2,"x",400
        """))
