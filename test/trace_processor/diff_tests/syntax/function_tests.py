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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto
from python.generators.diff_tests.testing import DiffTestBlueprint, ExpectedError
from python.generators.diff_tests.testing import TestSuite
from python.generators.diff_tests.testing import PrintProfileProto
from google.protobuf import text_format


class PerfettoFunction(TestSuite):

  # Usage of __intrinsic_zstd_compress: a string/blob compresses to a zstd frame
  # (asserted via the version-stable frame magic 0x28b52ffd rather than exact
  # bytes), and NULL passes through as NULL.
  def test_zstd_compress(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT
          hex(substr(__intrinsic_zstd_compress('hello, hello, hello'), 1, 4)) AS magic,
          length(__intrinsic_zstd_compress('hello, hello, hello')) > 0 AS has_output,
          __intrinsic_zstd_compress(NULL) IS NULL AS null_passthrough;
      """,
        out=Csv("""
        "magic","has_output","null_passthrough"
        "28B52FFD",1,1
      """))

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

  def test_legacy_create_function_rejects_recursion(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT create_function('f(x INT)', 'INT',
        '
          SELECT IIF($x = 0, 1, $x * f($x - 1))
        ');

        SELECT f(5);
      """,
        out=ExpectedError('f: recursive calls are not supported'))

  def test_create_function_variadic_delegate(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        CREATE PERFETTO FUNCTION my_hash(args ANY...) RETURNS INT
        DELEGATES TO hash;

        SELECT
          my_hash(1, 'hello', 42) as h1,
          my_hash('single_arg') as h2,
          my_hash(1, 2, 3, 4, 5) as h3;
      """,
        out=Csv("""
        "h1","h2","h3"
        4730678220997103486,1309196097616646754,-6396149620914793052
      """))