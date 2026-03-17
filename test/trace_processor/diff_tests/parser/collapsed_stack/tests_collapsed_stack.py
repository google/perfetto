#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class CollapsedStackParser(TestSuite):

  def test_collapsed_stack_simple_profile(self):
    return DiffTestBlueprint(
        trace=DataPath('collapsed_stack_simple.txt'),
        query="""
        SELECT scope, sample_type_type, sample_type_unit
        FROM __intrinsic_aggregate_profile
        ORDER BY scope, sample_type_type;
        """,
        out=Csv("""
        "scope","sample_type_type","sample_type_unit"
        "collapsed_stack_file","samples","count"
        """))

  def test_collapsed_stack_simple_samples(self):
    return DiffTestBlueprint(
        trace=DataPath('collapsed_stack_simple.txt'),
        query="""
        SELECT COUNT(*) as sample_count
        FROM __intrinsic_aggregate_sample;
        """,
        out=Csv("""
        "sample_count"
        3
        """))

  def test_collapsed_stack_simple_values(self):
    return DiffTestBlueprint(
        trace=DataPath('collapsed_stack_simple.txt'),
        query="""
        SELECT SUM(value) as total_count
        FROM __intrinsic_aggregate_sample;
        """,
        out=Csv("""
        "total_count"
        225.000000
        """))

  def test_collapsed_stack_frame_names(self):
    return DiffTestBlueprint(
        trace=DataPath('collapsed_stack_simple.txt'),
        query="""
        INCLUDE PERFETTO MODULE callstacks.stack_profile;
        SELECT DISTINCT c.name
        FROM _callstacks_for_stack_profile_samples!(
          (SELECT callsite_id FROM __intrinsic_aggregate_sample)
        ) c
        ORDER BY c.name;
        """,
        out=Csv("""
        "name"
        "bar"
        "baz"
        "foo"
        "main"
        "qux"
        """))

  def test_collapsed_stack_comments_ignored(self):
    return DiffTestBlueprint(
        trace=DataPath('collapsed_stack_with_comments.txt'),
        query="""
        SELECT COUNT(*) as sample_count
        FROM __intrinsic_aggregate_sample;
        """,
        out=Csv("""
        "sample_count"
        3
        """))

  def test_collapsed_stack_comments_total_value(self):
    return DiffTestBlueprint(
        trace=DataPath('collapsed_stack_with_comments.txt'),
        query="""
        SELECT SUM(value) as total_count
        FROM __intrinsic_aggregate_sample;
        """,
        out=Csv("""
        "total_count"
        1700.000000
        """))
