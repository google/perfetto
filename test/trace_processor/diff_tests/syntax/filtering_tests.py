#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

from python.generators.diff_tests.testing import DataPath, TextProto
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class PerfettoFiltering(TestSuite):
  # Below comparison tests are based on
  # https://www.sqlite.org/datatype3.html#comparisons.

  def test_comparison_sanity(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          count() AS cnt
        FROM slice;
        """,
        out=Csv("""
        "cnt"
        20746
        """))

  def test_comparison_ge_null(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          count() AS cnt
        FROM slice
        WHERE ts >= NULL;
        """,
        out=Csv("""
        "cnt"
        0
        """))

  def test_comparison_eq_null(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          count() AS cnt
        FROM slice
        WHERE dur = NULL;
        """,
        out=Csv("""
        "cnt"
        0
        """))

  def test_comparison_is_not_num(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          count() AS cnt
        FROM slice
        WHERE name IS NOT 3;
        """,
        out=Csv("""
        "cnt"
        20746
        """))

  def test_comparison_is_num(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          count() AS cnt
        FROM slice
        WHERE name IS 3;
        """,
        out=Csv("""
        "cnt"
        0
        """))

  def test_comparison_is_not_string(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          count() AS cnt
        FROM slice
        WHERE name IS NOT "Deoptimization JIT inline cache";
        """,
        out=Csv("""
        "cnt"
        20732
        """))

  def test_comparison_int_with_double(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          count() AS cnt
        FROM slice
        WHERE ts >= 72501936908.5;
        """,
        out=Csv("""
        "cnt"
        20696
        """))

  def test_comparison_string_ge_num(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          count() AS cnt
        FROM slice
        WHERE name >= 1.5;
        """,
        out=Csv("""
        "cnt"
        20746
        """))

  def test_int_is_less_than_text(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          count() AS cnt
        FROM slice
        WHERE ts < "cheese";
        """,
        out=Csv("""
        "cnt"
        20746
        """))

  def test_int_is_more_than_text(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          count() AS cnt
        FROM slice
        WHERE ts > "cheese";
        """,
        out=Csv("""
        "cnt"
        0
        """))

  def test_string_more_than_int(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          count() AS cnt
        FROM slice
        WHERE name > 3;
        """,
        out=Csv("""
        "cnt"
        20746
        """))

  def test_string_less_than_int(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          count() AS cnt
        FROM slice
        WHERE name < 3;
        """,
        out=Csv("""
        "cnt"
        0
        """))

  def test_string_null_vs_empty(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE foo AS
        SELECT 0 as id, NULL AS strings
        UNION ALL
        SELECT 1, 'cheese'
        UNION ALL
        SELECT 2, NULL
        UNION ALL
        SELECT 3, '';

        SELECT * FROM foo ORDER BY strings ASC;
        """,
        out=Csv("""
        "id","strings"
        0,"[NULL]"
        2,"[NULL]"
        3,""
        1,"cheese"
        """))

  def test_string_null_vs_empty_desc(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE foo AS
        SELECT 0 as id, NULL AS strings
        UNION ALL
        SELECT 1, 'cheese'
        UNION ALL
        SELECT 2, NULL
        UNION ALL
        SELECT 3, '';

        SELECT * FROM foo ORDER BY strings DESC;
        """,
        out=Csv("""
        "id","strings"
        1,"cheese"
        3,""
        0,"[NULL]"
        2,"[NULL]"
        """))

  def test_like_limit_one(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE foo AS
        SELECT 'foo' AS strings
        UNION ALL
        SELECT 'binder x'
        UNION ALL
        SELECT 'binder y'
        UNION ALL
        SELECT 'bar';

        SELECT * FROM foo WHERE strings LIKE '%binder%' LIMIT 1;
        """,
        out=Csv("""
        "strings"
        "binder x"
        """))

  def test_like_limit_multiple(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE foo AS
        SELECT 'foo' AS strings
        UNION ALL
        SELECT 'binder x'
        UNION ALL
        SELECT 'binder y'
        UNION ALL
        SELECT 'bar'
        UNION ALL
        SELECT 'binder z';

        SELECT * FROM foo WHERE strings LIKE '%binder%' LIMIT 2;
        """,
        out=Csv("""
        "strings"
        "binder x"
        "binder y"
        """))

  # Tests verifying IN gives the same results as equality for cross-type
  # filter values. IN is semantically "repeated equality", so
  # `col IN (x, y)` must match the same rows as `col = x OR col = y`.

  def test_in_int_col_with_double_exact_integer(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 10 AS val UNION ALL SELECT 20 UNION ALL SELECT 30;

        SELECT
          (SELECT count() FROM t WHERE val = 10.0) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (10.0)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        1,1
        """))

  def test_in_int_col_with_double_non_integer(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 10 AS val UNION ALL SELECT 20 UNION ALL SELECT 30;

        SELECT
          (SELECT count() FROM t WHERE val = 10.5) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (10.5)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        0,0
        """))

  def test_in_int_col_with_mixed_int_and_double(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 10 AS val UNION ALL SELECT 20 UNION ALL SELECT 30;

        SELECT
          (SELECT count() FROM t WHERE val = 10 OR val = 20.5) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (10, 20.5)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        1,1
        """))

  def test_in_int_col_with_string(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 10 AS val UNION ALL SELECT 20 UNION ALL SELECT 30;

        SELECT
          (SELECT count() FROM t WHERE val = 'hello') AS eq_cnt,
          (SELECT count() FROM t WHERE val IN ('hello')) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        0,0
        """))

  def test_in_int_col_with_mixed_int_and_string(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 10 AS val UNION ALL SELECT 20 UNION ALL SELECT 30;

        SELECT
          (SELECT count() FROM t WHERE val = 10 OR val = 'hello') AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (10, 'hello')) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        1,1
        """))

  def test_in_int_col_with_null(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 10 AS val UNION ALL SELECT 20 UNION ALL SELECT 30;

        SELECT
          (SELECT count() FROM t WHERE val = NULL) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (NULL)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        0,0
        """))

  def test_in_int_col_with_null_and_valid(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 10 AS val UNION ALL SELECT 20 UNION ALL SELECT 30;

        SELECT
          (SELECT count() FROM t WHERE val = 10 OR val = NULL) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (NULL, 10)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        1,1
        """))

  def test_in_double_col_with_int(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 1.0 AS val UNION ALL SELECT 2.5 UNION ALL SELECT 3.0;

        SELECT
          (SELECT count() FROM t WHERE val = 1) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (1)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        1,1
        """))

  def test_in_double_col_with_int_no_match(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 1.0 AS val UNION ALL SELECT 2.5 UNION ALL SELECT 3.0;

        SELECT
          (SELECT count() FROM t WHERE val = 2) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (2)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        0,0
        """))

  def test_in_double_col_with_mixed_int_and_double(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 1.0 AS val UNION ALL SELECT 2.5 UNION ALL SELECT 3.0;

        SELECT
          (SELECT count() FROM t WHERE val = 1 OR val = 2.5) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (1, 2.5)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        2,2
        """))

  def test_in_double_col_with_string(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 1.0 AS val UNION ALL SELECT 2.5 UNION ALL SELECT 3.0;

        SELECT
          (SELECT count() FROM t WHERE val = 'hello') AS eq_cnt,
          (SELECT count() FROM t WHERE val IN ('hello')) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        0,0
        """))

  def test_in_double_col_with_mixed_int_string_double(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 1.0 AS val UNION ALL SELECT 2.5 UNION ALL SELECT 3.0;

        SELECT
          (SELECT count() FROM t WHERE val = 1 OR val = 'hello'
           OR val = 2.5) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (1, 'hello', 2.5)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        2,2
        """))

  def test_in_string_col_with_int(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 'hello' AS val UNION ALL SELECT 'world' UNION ALL SELECT 'foo';

        SELECT
          (SELECT count() FROM t WHERE val = 1) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (1)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        0,0
        """))

  def test_in_string_col_with_mixed_string_and_int(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 'hello' AS val UNION ALL SELECT 'world' UNION ALL SELECT 'foo';

        SELECT
          (SELECT count() FROM t WHERE val = 'hello' OR val = 1) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN ('hello', 1)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        1,1
        """))

  def test_in_string_col_with_double(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 'hello' AS val UNION ALL SELECT 'world' UNION ALL SELECT 'foo';

        SELECT
          (SELECT count() FROM t WHERE val = 1.5) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (1.5)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        0,0
        """))

  def test_in_string_col_with_null(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 'hello' AS val UNION ALL SELECT 'world' UNION ALL SELECT 'foo';

        SELECT
          (SELECT count() FROM t WHERE val = NULL) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (NULL)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        0,0
        """))

  def test_in_string_col_with_mixed_string_int_double_null(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 'hello' AS val UNION ALL SELECT 'world' UNION ALL SELECT 'foo';

        SELECT
          (SELECT count() FROM t WHERE val = 'hello' OR val = 1
           OR val = 1.5 OR val = NULL) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN ('hello', 1, 1.5,
           NULL)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        1,1
        """))

  def test_in_nullable_int_col_with_mixed(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 10 AS val
        UNION ALL SELECT NULL
        UNION ALL SELECT 20
        UNION ALL SELECT NULL
        UNION ALL SELECT 30;

        SELECT
          (SELECT count() FROM t WHERE val = 10 OR val = 20.0
           OR val = 'hello') AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (10, 20.0, 'hello')) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        2,2
        """))

  def test_in_int_col_with_large_double(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 9007199254740992 AS val
        UNION ALL SELECT 9007199254740993;

        SELECT
          (SELECT count() FROM t WHERE val = 9007199254740992.0) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (9007199254740992.0)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        1,1
        """))

  def test_in_slice_int_col_with_double(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          (SELECT count() FROM slice WHERE ts = 72501936908.0) AS eq_cnt,
          (SELECT count() FROM slice WHERE ts IN (72501936908.0)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        1,1
        """))

  def test_in_slice_int_col_with_non_integer_double(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          (SELECT count() FROM slice WHERE ts = 72501936908.5) AS eq_cnt,
          (SELECT count() FROM slice WHERE ts IN (72501936908.5)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        0,0
        """))

  def test_in_slice_int_col_with_string(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          (SELECT count() FROM slice WHERE ts = 'cheese') AS eq_cnt,
          (SELECT count() FROM slice WHERE ts IN ('cheese')) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        0,0
        """))

  def test_in_slice_string_col_with_int(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          (SELECT count() FROM slice WHERE name = 3) AS eq_cnt,
          (SELECT count() FROM slice WHERE name IN (3)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        0,0
        """))

  def test_in_slice_string_col_with_mixed(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          (SELECT count() FROM slice
           WHERE name = 'Deoptimization JIT inline cache'
           OR name = 3 OR name = 1.5) AS eq_cnt,
          (SELECT count() FROM slice
           WHERE name IN ('Deoptimization JIT inline cache',
           3, 1.5)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        14,14
        """))

  def test_in_slice_int_col_with_mixed(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT
          (SELECT count() FROM slice
           WHERE ts = 72501936908 OR ts = 72501936908.5
           OR ts = 'cheese') AS eq_cnt,
          (SELECT count() FROM slice
           WHERE ts IN (72501936908, 72501936908.5,
           'cheese')) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        1,1
        """))

  # Edge cases: CAST to force specific types across type boundaries.

  def test_in_int_col_with_cast_int_to_real(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 10 AS val UNION ALL SELECT 20 UNION ALL SELECT 30;

        SELECT
          (SELECT count() FROM t WHERE val = CAST(10 AS REAL)) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (CAST(10 AS REAL))) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        1,1
        """))

  def test_in_double_col_with_cast_real_to_int(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 1.0 AS val UNION ALL SELECT 2.5 UNION ALL SELECT 3.0;

        SELECT
          (SELECT count() FROM t WHERE val = CAST(1.0 AS INTEGER)) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (CAST(1.0 AS INTEGER))) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        1,1
        """))

  # Large IN lists to trigger hash lookup path (>16 elements).

  def test_in_int_col_large_list_with_trailing_double(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        WITH RECURSIVE s(x) AS (
          SELECT 1 UNION ALL SELECT x + 1 FROM s WHERE x < 100
        )
        SELECT x AS val FROM s;

        SELECT
          (SELECT count() FROM t WHERE val = 1 OR val = 2 OR val = 3
           OR val = 4 OR val = 5 OR val = 6 OR val = 7 OR val = 8
           OR val = 9 OR val = 10 OR val = 11 OR val = 12 OR val = 13
           OR val = 14 OR val = 15 OR val = 16 OR val = 17 OR val = 18
           OR val = 50.0) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (1,2,3,4,5,6,7,8,9,10,
           11,12,13,14,15,16,17,18,50.0)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        19,19
        """))

  def test_in_int_col_large_list_with_non_integer_double(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        WITH RECURSIVE s(x) AS (
          SELECT 1 UNION ALL SELECT x + 1 FROM s WHERE x < 100
        )
        SELECT x AS val FROM s;

        SELECT
          (SELECT count() FROM t WHERE val = 1 OR val = 2 OR val = 3
           OR val = 4 OR val = 5 OR val = 6 OR val = 7 OR val = 8
           OR val = 9 OR val = 10 OR val = 11 OR val = 12 OR val = 13
           OR val = 14 OR val = 15 OR val = 16 OR val = 17 OR val = 18
           OR val = 50.5) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (1,2,3,4,5,6,7,8,9,10,
           11,12,13,14,15,16,17,18,50.5)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        18,18
        """))

  def test_in_int_col_large_list_with_string(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        WITH RECURSIVE s(x) AS (
          SELECT 1 UNION ALL SELECT x + 1 FROM s WHERE x < 100
        )
        SELECT x AS val FROM s;

        SELECT
          (SELECT count() FROM t WHERE val = 1 OR val = 2 OR val = 3
           OR val = 4 OR val = 5 OR val = 6 OR val = 7 OR val = 8
           OR val = 9 OR val = 10 OR val = 11 OR val = 12 OR val = 13
           OR val = 14 OR val = 15 OR val = 16 OR val = 17 OR val = 18
           OR val = 'hello') AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (1,2,3,4,5,6,7,8,9,10,
           11,12,13,14,15,16,17,18,'hello')) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        18,18
        """))

  def test_in_int_col_large_list_all_doubles(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        WITH RECURSIVE s(x) AS (
          SELECT 1 UNION ALL SELECT x + 1 FROM s WHERE x < 100
        )
        SELECT x AS val FROM s;

        SELECT
          (SELECT count() FROM t WHERE val = 1.0 OR val = 2.0 OR val = 3.0
           OR val = 4.0 OR val = 5.0 OR val = 6.0 OR val = 7.0 OR val = 8.0
           OR val = 9.0 OR val = 10.0 OR val = 11.0 OR val = 12.0
           OR val = 13.0 OR val = 14.0 OR val = 15.0 OR val = 16.0
           OR val = 17.0 OR val = 18.0) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (1.0,2.0,3.0,4.0,5.0,
           6.0,7.0,8.0,9.0,10.0,11.0,12.0,13.0,14.0,15.0,16.0,
           17.0,18.0)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        18,18
        """))

  # Precision boundary: int64 values near 2^53 (double precision limit).

  def test_in_int_col_precision_boundary(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 9007199254740992 AS val
        UNION ALL SELECT 9007199254740993;

        SELECT
          (SELECT count() FROM t WHERE val = 9007199254740992.0) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (9007199254740992.0)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        1,1
        """))

  # Negative zero handling for double columns.

  def test_in_double_col_negative_zero(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE t AS
        SELECT 0.0 AS val UNION ALL SELECT 1.0 UNION ALL SELECT -1.0;

        SELECT
          (SELECT count() FROM t WHERE val = -0.0) AS eq_cnt,
          (SELECT count() FROM t WHERE val IN (-0.0)) AS in_cnt;
        """,
        out=Csv("""
        "eq_cnt","in_cnt"
        1,1
        """))
