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


class PerfettoFunction(TestSuite):

  def test_create_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO FUNCTION f(x INT) RETURNS INT AS SELECT $x + 1;

        SELECT f(5) as result;
      """,
        out=Csv("""
        "result"
        6
      """))

  def test_replace_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO FUNCTION f(x INT) RETURNS INT AS SELECT $x + 1;
        CREATE OR REPLACE PERFETTO FUNCTION f(x INT) RETURNS INT AS SELECT $x + 2;

        SELECT f(5) as result;
      """,
        out=Csv("""
        "result"
        7
      """))

  def test_legacy_create_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT create_function('f(x INT)', 'INT', 'SELECT $x + 1');

        SELECT f(5) as result;
      """,
        out=Csv("""
        "result"
        6
      """))

  def test_legacy_create_function_returns_string(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT create_function('f(x INT)', 'STRING', 'SELECT "value_" || $x');

        SELECT f(5) as result;
      """,
        out=Csv("""
        "result"
        "value_5"
      """))

  def test_legacy_create_function_duplicated(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT create_function('f()', 'INT', 'SELECT 1');
        SELECT create_function('f()', 'INT', 'SELECT 1');

        SELECT f() as result;
      """,
        out=Csv("""
        "result"
        1
      """))

  def test_legacy_create_function_recursive(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        -- Compute factorial.
        SELECT create_function('f(x INT)', 'INT',
        '
          SELECT IIF($x = 0, 1, $x * f($x - 1))
        ');

        SELECT f(5) as result;
      """,
        out=Csv("""
        "result"
        120
      """))

  def test_legacy_create_function_recursive_string(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        -- Compute factorial.
        SELECT create_function('f(x INT)', 'STRING',
        '
          SELECT IIF(
            $x = 0,
            "",
            -- 97 is the ASCII code for "a".
            f($x - 1) || char(96 + $x) || f($x - 1))
        ');

        SELECT f(4) as result;
      """,
        out=Csv("""
          "result"
          "abacabadabacaba"
      """))

  def test_legacy_create_function_recursive_string_memoized(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        -- Compute factorial.
        SELECT create_function('f(x INT)', 'STRING',
        '
          SELECT IIF(
            $x = 0,
            "",
            -- 97 is the ASCII code for "a".
            f($x - 1) || char(96 + $x) || f($x - 1))
        ');

        SELECT experimental_memoize('f');

        SELECT f(4) as result;
      """,
        out=Csv("""
          "result"
          "abacabadabacaba"
      """))

  def test_legacy_create_function_memoize(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        -- Compute 2^n inefficiently to test memoization.
        -- If it times out, memoization is not working.
        SELECT create_function('f(x INT)', 'INT',
        '
          SELECT IIF($x = 0, 1, f($x - 1) + f($x - 1))
        ');

        SELECT EXPERIMENTAL_MEMOIZE('f');

        -- 2^50 is too expensive to compute, but memoization makes it fast.
        SELECT f(50) as result;
      """,
        out=Csv("""
        "result"
        1125899906842624
      """))

  def test_legacy_create_function_memoize_float(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        -- Compute 2^n inefficiently to test memoization.
        -- If it times out, memoization is not working.
        SELECT create_function('f(x INT)', 'FLOAT',
        '
          SELECT $x + 0.5
        ');

        SELECT EXPERIMENTAL_MEMOIZE('f');

        SELECT printf("%.1f", f(1)) as result
        UNION ALL
        SELECT printf("%.1f", f(1)) as result
        UNION ALL
        SELECT printf("%.1f", f(1)) as result
      """,
        out=Csv("""
        "result"
        "1.5"
        "1.5"
        "1.5"
      """))

  def test_legacy_create_function_memoize_intermittent_memoization(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        -- This function returns NULL for odd numbers and 1 for even numbers.
        -- As we do not memoize NULL results, we would only memoize the results
        -- for even numbers.
        SELECT create_function('f(x INT)', 'INT',
        '
          SELECT IIF($x = 0, 1,
            IIF(f($x - 1) IS NULL, 1, NULL)
          )
        ');

        SELECT EXPERIMENTAL_MEMOIZE('f');

        SELECT
          f(50) as f_50,
          f(51) as f_51;
      """,
        out=Csv("""
        "f_50","f_51"
        1,"[NULL]"
      """))

  def test_legacy_create_function_memoize_subtree_size(self):
    # Tree:
    #            1
    #           / \
    #          /   \
    #         /     \
    #        2       3
    #       / \     / \
    #      4   5   6   7
    #     / \  |   |  | \
    #    8   9 10 11 12 13
    #    |   |
    #   14   15
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO TABLE tree AS
        WITH data(id, parent_id) as (VALUES
          (1, NULL),
          (2, 1),
          (3, 1),
          (4, 2),
          (5, 2),
          (6, 3),
          (7, 3),
          (8, 4),
          (9, 4),
          (10, 5),
          (11, 6),
          (12, 7),
          (13, 7),
          (14, 8),
          (15, 9)
        )
        SELECT * FROM data;

        SELECT create_function('subtree_size(id INT)', 'INT',
        '
          SELECT 1 + IFNULL((
            SELECT
              SUM(subtree_size(child.id))
            FROM tree child
            WHERE child.parent_id = $id
          ), 0)
        ');

        SELECT EXPERIMENTAL_MEMOIZE('subtree_size');

        SELECT
          id, subtree_size(id) as size
        FROM tree
        ORDER BY id;
      """,
        out=Csv("""
        "id","size"
        1,15
        2,8
        3,6
        4,5
        5,2
        6,2
        7,3
        8,2
        9,2
        10,1
        11,1
        12,1
        13,1
        14,1
        15,1
      """))