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


class CreateSlices(TestSuite):

  def test_create_slices_basic(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.create_slices;
        WITH starts(ts) AS (
          VALUES (100), (200), (300)
        ),
        ends(ts) AS (
          VALUES (150), (250), (350)
        )
        SELECT * FROM _create_slices!(starts, ends, ts, ts)
        """,
        out=Csv("""
        "ts","dur"
        100,50
        200,50
        300,50
        """))

  def test_create_slices_multiple_starts_same_end(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.create_slices;
        WITH starts(ts) AS (
          VALUES (100), (200), (300)
        ),
        ends(ts) AS (
          VALUES (500)
        )
        SELECT * FROM _create_slices!(starts, ends, ts, ts)
        """,
        out=Csv("""
        "ts","dur"
        100,400
        200,300
        300,200
        """))

  def test_create_slices_no_matching_ends(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.create_slices;
        WITH starts(ts) AS (
          VALUES (500), (600)
        ),
        ends(ts) AS (
          VALUES (100), (200)
        )
        SELECT * FROM _create_slices!(starts, ends, ts, ts)
        """,
        out=Csv("""
        "ts","dur"
        """))

  def test_create_slices_interleaved(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.create_slices;
        WITH starts(ts) AS (
          VALUES (10), (30), (50)
        ),
        ends(ts) AS (
          VALUES (20), (40), (60)
        )
        SELECT * FROM _create_slices!(starts, ends, ts, ts)
        """,
        out=Csv("""
        "ts","dur"
        10,10
        30,10
        50,10
        """))

  def test_create_slices_partial_match(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.create_slices;
        WITH starts(ts) AS (
          VALUES (10), (30), (50), (70)
        ),
        ends(ts) AS (
          VALUES (25), (55)
        )
        SELECT * FROM _create_slices!(starts, ends, ts, ts)
        """,
        out=Csv("""
        "ts","dur"
        10,15
        30,25
        50,5
        """))

  def test_create_slices_empty_starts(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.create_slices;
        WITH starts(ts) AS (
          SELECT 0 WHERE 0
        ),
        ends(ts) AS (
          VALUES (100), (200)
        )
        SELECT * FROM _create_slices!(starts, ends, ts, ts)
        """,
        out=Csv("""
        "ts","dur"
        """))

  def test_create_slices_equal_start_and_end(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.create_slices;
        WITH starts(ts) AS (
          VALUES (100), (200)
        ),
        ends(ts) AS (
          VALUES (100), (200), (300)
        )
        SELECT * FROM _create_slices!(starts, ends, ts, ts)
        """,
        out=Csv("""
        "ts","dur"
        100,100
        200,100
        """))

  def test_create_slices_unsorted_input(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.create_slices;
        WITH starts(ts) AS (
          VALUES (300), (100), (200)
        ),
        ends(ts) AS (
          VALUES (350), (150), (250)
        )
        SELECT * FROM _create_slices!(starts, ends, ts, ts)
        """,
        out=Csv("""
        "ts","dur"
        100,50
        200,50
        300,50
        """))
