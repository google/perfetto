#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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


class IntervalsIntersect(TestSuite):

  def test_simple_inteval_intersect(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        #      0 1 2 3 4 5 6 7
        # A:   _ - - - - - - _
        # B:   - - _ - - _ - -
        # res: _ - _ - - _ - _
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE A AS
          WITH data(id, ts, dur) AS (
            VALUES
            (0, 1, 6)
          )
          SELECT * FROM data;

        CREATE PERFETTO TABLE B AS
          WITH data(id, ts, dur) AS (
            VALUES
            (0, 0, 2),
            (1, 3, 2),
            (2, 6, 2)
          )
          SELECT * FROM data;

        SELECT ts, dur, left_id, right_id
        FROM _interval_intersect!(A, B)
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","left_id","right_id"
        1,1,0,0
        3,2,0,1
        6,1,0,2
        """))

  def test_simple_inteval_intersect_rev(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        #      0 1 2 3 4 5 6 7
        # A:   _ - - - - - - _
        # B:   - - _ - - _ - -
        # res: _ - _ - - _ - _
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE A AS
          WITH data(id, ts, dur) AS (
            VALUES
            (0, 1, 6)
          )
          SELECT * FROM data;

        CREATE PERFETTO TABLE B AS
          WITH data(id, ts, dur) AS (
            VALUES
            (0, 0, 2),
            (1, 3, 2),
            (2, 6, 2)
          )
          SELECT * FROM data;

        SELECT ts, dur, left_id, right_id
        FROM _interval_intersect!(B, A)
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","left_id","right_id"
        1,1,0,0
        3,2,1,0
        6,1,2,0
        """))

  def test_no_overlap(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        # A:   __-
        # B:   -__
        # res: ___
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE A AS
          WITH data(id, ts, dur) AS (
            VALUES
            (0, 2, 1)
          )
          SELECT * FROM data;

        CREATE PERFETTO TABLE B AS
          WITH data(id, ts, dur) AS (
            VALUES
            (0, 0, 1)
          )
          SELECT * FROM data;

        SELECT ts, dur, left_id, right_id
        FROM _interval_intersect!(A, B)
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","left_id","right_id"
        """))

  def test_no_overlap_rev(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        # A:   __-
        # B:   -__
        # res: ___
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE A AS
          WITH data(id, ts, dur) AS (
            VALUES
            (0, 2, 1)
          )
          SELECT * FROM data;

        CREATE PERFETTO TABLE B AS
          WITH data(id, ts, dur) AS (
            VALUES
            (0, 0, 1)
          )
          SELECT * FROM data;

        SELECT ts, dur, left_id, right_id
        FROM _interval_intersect!(B, A)
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","left_id","right_id"
        """))

  def test_single_point_overlap(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        # A:   _-
        # B:   -_
        # res: __
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE A AS
          WITH data(id, ts, dur) AS (
            VALUES
            (0, 1, 1)
          )
          SELECT * FROM data;

        CREATE PERFETTO TABLE B AS
          WITH data(id, ts, dur) AS (
            VALUES
            (0, 0, 1)
          )
          SELECT * FROM data;

        SELECT ts, dur, left_id, right_id
        FROM _interval_intersect!(A, B)
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","left_id","right_id"
        """))

  def test_single_point_overlap_rev(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        # A:   _-
        # B:   -_
        # res: __
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE A AS
          WITH data(id, ts, dur) AS (
            VALUES
            (0, 1, 1)
          )
          SELECT * FROM data;

        CREATE PERFETTO TABLE B AS
          WITH data(id, ts, dur) AS (
            VALUES
            (0, 0, 1)
          )
          SELECT * FROM data;

        SELECT ts, dur, left_id, right_id
        FROM _interval_intersect!(B, A)
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","left_id","right_id"
        """))