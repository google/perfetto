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

from python.generators.diff_tests.testing import DataPath
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
