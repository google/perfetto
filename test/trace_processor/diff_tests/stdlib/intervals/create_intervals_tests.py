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


class CreateIntervals(TestSuite):

  def test_create_intervals_basic(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.create_intervals;
        WITH starts(id, ts) AS (
          VALUES (10, 100), (11, 200), (12, 300)
        ),
        ends(id, ts) AS (
          VALUES (20, 150), (21, 250), (22, 350)
        )
        SELECT * FROM _interval_create!(starts, ends)
        """,
        out=Csv("""
        "ts","dur","start_id","end_id"
        100,50,10,20
        200,50,11,21
        300,50,12,22
        """))

  def test_create_intervals_multiple_starts_same_end(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.create_intervals;
        WITH starts(id, ts) AS (
          VALUES (10, 100), (11, 200), (12, 300)
        ),
        ends(id, ts) AS (
          VALUES (20, 500)
        )
        SELECT * FROM _interval_create!(starts, ends)
        """,
        out=Csv("""
        "ts","dur","start_id","end_id"
        100,400,10,20
        200,300,11,20
        300,200,12,20
        """))

  def test_create_intervals_no_matching_ends(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.create_intervals;
        WITH starts(id, ts) AS (
          VALUES (10, 500), (11, 600)
        ),
        ends(id, ts) AS (
          VALUES (20, 100), (21, 200)
        )
        SELECT * FROM _interval_create!(starts, ends)
        """,
        out=Csv("""
        "ts","dur","start_id","end_id"
        """))

  def test_create_intervals_interleaved(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.create_intervals;
        WITH starts(id, ts) AS (
          VALUES (1, 10), (2, 30), (3, 50)
        ),
        ends(id, ts) AS (
          VALUES (4, 20), (5, 40), (6, 60)
        )
        SELECT * FROM _interval_create!(starts, ends)
        """,
        out=Csv("""
        "ts","dur","start_id","end_id"
        10,10,1,4
        30,10,2,5
        50,10,3,6
        """))

  def test_create_intervals_partial_match(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.create_intervals;
        WITH starts(id, ts) AS (
          VALUES (1, 10), (2, 30), (3, 50), (4, 70)
        ),
        ends(id, ts) AS (
          VALUES (5, 25), (6, 55)
        )
        SELECT * FROM _interval_create!(starts, ends)
        """,
        out=Csv("""
        "ts","dur","start_id","end_id"
        10,15,1,5
        30,25,2,6
        50,5,3,6
        """))

  def test_create_intervals_empty_starts(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.create_intervals;
        WITH starts(id, ts) AS (
          SELECT 0, 0 WHERE 0
        ),
        ends(id, ts) AS (
          VALUES (1, 100), (2, 200)
        )
        SELECT * FROM _interval_create!(starts, ends)
        """,
        out=Csv("""
        "ts","dur","start_id","end_id"
        """))

  def test_create_intervals_equal_start_and_end(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.create_intervals;
        WITH starts(id, ts) AS (
          VALUES (1, 100), (2, 200)
        ),
        ends(id, ts) AS (
          VALUES (3, 100), (4, 200), (5, 300)
        )
        SELECT * FROM _interval_create!(starts, ends)
        """,
        out=Csv("""
        "ts","dur","start_id","end_id"
        100,100,1,4
        200,100,2,5
        """))

  def test_create_intervals_unsorted_input(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.create_intervals;
        WITH starts(id, ts) AS (
          VALUES (1, 300), (2, 100), (3, 200)
        ),
        ends(id, ts) AS (
          VALUES (4, 350), (5, 150), (6, 250)
        )
        SELECT * FROM _interval_create!(starts, ends)
        """,
        out=Csv("""
        "ts","dur","start_id","end_id"
        100,50,2,5
        200,50,3,6
        300,50,1,4
        """))
