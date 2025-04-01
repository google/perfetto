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


class StdlibIntervals(TestSuite):

  def test_intervals_overlap_count(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.overlap;

        WITH data(ts, dur) AS (
          VALUES
            (10, 40),
            (20, 10),
            (25, 10),
            (60, 10),
            (70, 20),
            (80, -1)
        )
        SELECT *
        FROM intervals_overlap_count!(data, ts, dur)
        """,
        out=Csv("""
        "ts","value"
        10,1
        20,2
        25,3
        30,2
        35,1
        50,0
        60,1
        70,1
        80,2
        90,1
        """))

  def test_intervals_overlap_count_by_group(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.overlap;

        WITH data(ts, dur, group_name) AS (
          VALUES
            (10, 40, "A"),
            (15, 30, "B"),
            (20, 10, "A"),
            (25, 10, "B"),
            (30, 10, "B"),
            (60, 10, "A"),
            (60, -1, "B"),
            (70, 20, "A"),
            (80, -1, "A")
        )
        SELECT *
        FROM intervals_overlap_count_by_group!(data, ts, dur, group_name)
        """,
        out=Csv("""
        "ts","value","group_name"
        10,1,"A"
        15,1,"B"
        20,2,"A"
        25,2,"B"
        30,1,"A"
        30,3,"B"
        35,2,"B"
        40,1,"B"
        45,0,"B"
        50,0,"A"
        60,1,"A"
        60,1,"B"
        70,1,"A"
        80,2,"A"
        90,1,"A"
        """))

  def test_intervals_overlap_in_table(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.overlap;

        WITH data_no_overlaps(ts, dur) AS (
          VALUES
            (10, 10),
            (30, 10)
        ),
        data_with_overlaps(ts, dur) AS (
          VALUES
            (10, 10),
            (15, 10)
        )
        SELECT * FROM (
        SELECT *
        FROM _intervals_overlap_in_table!(data_no_overlaps)
        UNION
        SELECT *
        FROM _intervals_overlap_in_table!(data_with_overlaps)
        )
        """,
        out=Csv("""
        "has_overlaps"
        0
        1
        """))

  def test_intervals_flatten(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.overlap;

        WITH roots_data (id, ts, dur) AS (
          VALUES
            (0, 0, 9),
            (1, 9, 1)
        ), children_data (root_id, id, parent_id, ts, dur) AS (
          VALUES
            (0, 2, 0, 1, 3),
            (0, 3, 0, 5, 1),
            (0, 4, 0, 6, 1),
            (0, 5, 0, 7, 0),
            (0, 6, 0, 7, 1),
            (0, 7, 2, 2, 1)
        )
        SELECT ts, dur, id, root_id
        FROM _intervals_flatten!(_intervals_merge_root_and_children!(roots_data, children_data)) ORDER BY ts
        """,
        out=Csv("""
        "ts","dur","id","root_id"
        0,1,0,0
        1,1,2,0
        2,1,7,0
        3,1,2,0
        4,1,0,0
        5,1,3,0
        6,1,4,0
        7,1,6,0
        8,1,0,0
        9,1,1,1
        """))

  def test_intervals_flatten_by_intersection(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.overlap;

        CREATE PERFETTO TABLE foo AS
        WITH roots_data (id, ts, dur, utid) AS (
          VALUES
            (0, 0, 9, 0),
            (0, 0, 9, 1),
            (1, 9, 1, 2)
        ), children_data (id, parent_id, ts, dur, utid) AS (
          VALUES
            (2, 0, 1, 3, 0),
            (3, 0, 5, 1, 0),
            (4, 0, 6, 1, 0),
            (5, 0, 7, 0, 0),
            (6, 0, 7, 1, 0),
            (7, 2, 2, 1, 0)
        )
        SELECT *
        FROM _intervals_merge_root_and_children_by_intersection!(roots_data, children_data, utid);

        SELECT ts, dur, id, root_id FROM _intervals_flatten!(foo) ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id","root_id"
        0,1,0,0
        1,1,2,0
        2,1,7,0
        3,1,2,0
        4,1,0,0
        5,1,3,0
        6,1,4,0
        7,1,6,0
        8,1,0,0
        """))

  def test_intervals_flatten_by_intersection_no_matching_key(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.overlap;

        CREATE PERFETTO TABLE foo AS
        WITH roots_data (id, ts, dur, utid) AS (
          VALUES
            (0, 0, 9, 1),
            (0, 0, 9, 2),
            (1, 9, 1, 3)
        ), children_data (id, parent_id, ts, dur, utid) AS (
          VALUES
            (2, 0, 1, 3, 0),
            (3, 0, 5, 1, 0),
            (4, 0, 6, 1, 0),
            (5, 0, 7, 0, 0),
            (6, 0, 7, 1, 0),
            (7, 2, 2, 1, 0)
        )
        SELECT *
        FROM _intervals_merge_root_and_children_by_intersection!(roots_data, children_data, utid);

        SELECT ts, dur, id, root_id FROM _intervals_flatten!(foo) ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id","root_id"
        """))


  def test_interval_merge_overlapping(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.overlap;

        WITH
          data(ts, dur) AS (
            VALUES
              -- partial overlap
              (1, 4),
              (2, 4),
              -- end within epsilon of start
              (10, 3),
              (14, 2),
              -- end not within epsilon of start
              (20, 3),
              (26, 2),
              -- nested
              (30, 4),
              (31, 2)
          )
        SELECT *
        FROM interval_merge_overlapping!(data, 1);
        """,
        out=Csv("""
        "ts","dur"
        1,5
        10,6
        20,3
        26,2
        30,4
        """))

  def test_intersect_list(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        WITH
          data(ts, dur, id) AS (
            VALUES
              (10, 100, 0),
              (20, 40, 1),
              (30, 120, 2),
              (200, 10, 3),
              (200, 20, 4),
              (300, 10, 5)
          )
        SELECT *
        FROM interval_self_intersect!(data)
        ORDER BY ts ASC, id ASC;
        """,
        out=Csv("""
        "ts","dur","group_id","id","interval_ends_at_ts"
        10,10,1,0,0
        20,10,2,0,0
        20,10,2,1,0
        30,30,3,0,0
        30,30,3,1,0
        30,30,3,2,0
        60,50,4,0,0
        60,50,4,1,1
        60,50,4,2,0
        110,40,5,0,1
        110,40,5,2,0
        150,50,6,2,1
        200,10,7,3,0
        200,10,7,4,0
        210,10,8,3,1
        210,10,8,4,0
        220,80,9,4,1
        300,10,10,5,0
        310,0,11,5,1
        """))
