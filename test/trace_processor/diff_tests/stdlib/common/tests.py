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
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class StdlibCommon(TestSuite):

  def test_spans_overlapping_dur_intersect_edge(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        INCLUDE PERFETTO MODULE common.timestamps;
        SELECT SPANS_OVERLAPPING_DUR(0, 2, 1, 2) AS dur
        """,
        out=Csv("""
        "dur"
        1
        """))

  def test_spans_overlapping_dur_intersect_edge_reversed(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        INCLUDE PERFETTO MODULE common.timestamps;
        SELECT SPANS_OVERLAPPING_DUR(1, 2, 0, 2) AS dur
        """,
        out=Csv("""
        "dur"
        1
        """))

  def test_spans_overlapping_dur_intersect_all(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        INCLUDE PERFETTO MODULE common.timestamps;
        SELECT SPANS_OVERLAPPING_DUR(0, 3, 1, 1) AS dur
        """,
        out=Csv("""
        "dur"
        1
        """))

  def test_spans_overlapping_dur_intersect_all_reversed(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        INCLUDE PERFETTO MODULE common.timestamps;
        SELECT SPANS_OVERLAPPING_DUR(1, 1, 0, 3) AS dur
        """,
        out=Csv("""
        "dur"
        1
        """))

  def test_spans_overlapping_dur_no_intersect(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        INCLUDE PERFETTO MODULE common.timestamps;
        SELECT SPANS_OVERLAPPING_DUR(0, 1, 2, 1) AS dur
        """,
        out=Csv("""
        "dur"
        0
        """))

  def test_spans_overlapping_dur_no_intersect_reversed(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        INCLUDE PERFETTO MODULE common.timestamps;
        SELECT SPANS_OVERLAPPING_DUR(2, 1, 0, 1) AS dur
        """,
        out=Csv("""
        "dur"
        0
        """))

  def test_spans_overlapping_dur_negative_dur(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        INCLUDE PERFETTO MODULE common.timestamps;
        SELECT SPANS_OVERLAPPING_DUR(0, -1, 0, 1) AS dur
        """,
        out=Csv("""
        "dur"
        0
        """))

  def test_spans_overlapping_dur_negative_dur_reversed(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        INCLUDE PERFETTO MODULE common.timestamps;
        SELECT SPANS_OVERLAPPING_DUR(0, 1, 0, -1) AS dur
        """,
        out=Csv("""
        "dur"
        0
        """))
