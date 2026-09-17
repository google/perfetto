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


class IntervalsSelfIntersect(TestSuite):

  # Mirrors test_intersect_list in intervals/tests.py: same input, same
  # expected output. Confirms the C++ _interval_self_intersect in
  # intervals.self_intersect is a drop-in for the SQL stdlib's
  # intervals.intersect.interval_self_intersect.
  def test_self_intersect_list(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

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
        FROM _interval_self_intersect!(data)
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

  # Adjacent intervals: end of one coincides with start of the next.
  # Active set should not include the ending interval in segments at/after
  # its end ts; the end marker fires at the segment beginning at the end ts.
  def test_adjacent_intervals(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        WITH
          data(ts, dur, id) AS (
            VALUES
              (0, 10, 0),
              (10, 10, 1)
          )
        SELECT *
        FROM _interval_self_intersect!(data)
        ORDER BY ts ASC, id ASC;
        """,
        out=Csv("""
        "ts","dur","group_id","id","interval_ends_at_ts"
        0,10,1,0,0
        10,10,2,0,1
        10,10,2,1,0
        20,0,3,1,1
        """))

  # Empty input table: zero rows out, no error.
  def test_empty_input(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        WITH data(ts, dur, id) AS (SELECT 0, 0, 0 WHERE FALSE)
        SELECT * FROM _interval_self_intersect!(data);
        """,
        out=Csv("""
        "ts","dur","group_id","id","interval_ends_at_ts"
        """))

  # --- _interval_self_intersect_count / _interval_self_intersect_agg ---
  # group_id is hash-map iteration order across partitions (stable only
  # within a partition), so tests exclude it except where a single
  # partition makes it deterministic.

  def test_self_intersect_count_partitioned(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        WITH
          data(ts, dur, k0) AS (
            VALUES
              (0, 100, 'A'),
              (10, 50, 'A'),
              (20, 100, 'B'),
              (30, 20, 'B')
          )
        SELECT ts, dur, cnt, k0
        FROM _interval_self_intersect_count!(data, (k0))
        ORDER BY k0 ASC, ts ASC;
        """,
        out=Csv("""
        "ts","dur","cnt","k0"
        0,10,1,"A"
        10,50,2,"A"
        60,40,1,"A"
        100,0,0,"A"
        20,10,1,"B"
        30,20,2,"B"
        50,70,1,"B"
        120,0,0,"B"
        """))

  def test_self_intersect_count_gap_drops_to_zero(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        WITH
          data(ts, dur, k0) AS (
            VALUES
              (0, 10, 'X'),
              (20, 10, 'X')
          )
        SELECT ts, dur, group_id, cnt, k0
        FROM _interval_self_intersect_count!(data, (k0))
        ORDER BY ts ASC;
        """,
        out=Csv("""
        "ts","dur","group_id","cnt","k0"
        0,10,1,1,"X"
        10,10,2,0,"X"
        20,10,3,1,"X"
        30,0,4,0,"X"
        """))

  def test_self_intersect_agg_values(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        WITH
          data(ts, dur, v, k0) AS (
            VALUES
              (0, 10, 10, 'P'),
              (5, 10, NULL, 'P'),
              (8, 4, 2, 'P')
          )
        SELECT
          ts, dur, cnt,
          CAST(sum_value AS INT64) AS sum_v,
          CAST(min_value AS INT64) AS min_v,
          CAST(max_value AS INT64) AS max_v,
          k0
        FROM _interval_self_intersect_agg!(data, v, (k0))
        ORDER BY ts ASC;
        """,
        out=Csv("""
        "ts","dur","cnt","sum_v","min_v","max_v","k0"
        0,5,1,10,10,10,"P"
        5,3,2,10,10,10,"P"
        8,2,3,12,2,10,"P"
        10,2,2,2,2,2,"P"
        12,3,1,0,"[NULL]","[NULL]","P"
        15,0,0,0,"[NULL]","[NULL]","P"
        """))

  def test_self_intersect_count_null_partition_key(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        WITH
          data(ts, dur, k0) AS (
            VALUES
              (0, 10, NULL),
              (5, 10, NULL),
              (2, 6, 'X')
          )
        SELECT ts, dur, cnt, k0
        FROM _interval_self_intersect_count!(data, (k0))
        ORDER BY k0 ASC, ts ASC;
        """,
        out=Csv("""
        "ts","dur","cnt","k0"
        0,5,1,"[NULL]"
        5,5,2,"[NULL]"
        10,5,1,"[NULL]"
        15,0,0,"[NULL]"
        2,6,1,"X"
        8,0,0,"X"
        """))

  def test_self_intersect_count_multi_key(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        WITH
          data(ts, dur, p, q) AS (
            VALUES
              (0, 10, 'A', 1),
              (5, 10, 'A', 1),
              (5, 10, 'B', 1),
              (10, 5, 'B', 2)
          )
        SELECT ts, dur, cnt, p, q
        FROM _interval_self_intersect_count!(data, (p, q))
        ORDER BY p ASC, q ASC, ts ASC;
        """,
        out=Csv("""
        "ts","dur","cnt","p","q"
        0,5,1,"A",1
        5,5,2,"A",1
        10,5,1,"A",1
        15,0,0,"A",1
        5,10,1,"B",1
        15,0,0,"B",1
        10,5,1,"B",2
        15,0,0,"B",2
        """))

  def test_self_intersect_count_zero_dur_intervals(self):
    # The zero-dur interval at ts=5 is never active in any segment, so the
    # boundary it creates changes no aggregate and the [0,5) and [5,10)
    # segments merge into one row.
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        WITH
          data(ts, dur, k0) AS (
            VALUES
              (0, 10, 'Y'),
              (5, 0, 'Y'),
              (7, 0, 'Z')
          )
        SELECT ts, dur, cnt, k0
        FROM _interval_self_intersect_count!(data, (k0))
        ORDER BY k0 ASC, ts ASC;
        """,
        out=Csv("""
        "ts","dur","cnt","k0"
        0,10,1,"Y"
        10,0,0,"Y"
        7,0,0,"Z"
        """))

  def test_self_intersect_agg_merges_touching_intervals(self):
    # T: back-to-back intervals with equal values keep every aggregate
    # constant across the ts=10 boundary, so a single merged row spans both.
    # W: the boundary changes sum/min/max (1 vs 2) even though cnt stays 1,
    # so no merge happens there.
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        WITH
          data(ts, dur, v, k0) AS (
            VALUES
              (0, 10, 5, 'T'),
              (10, 10, 5, 'T'),
              (0, 10, 1, 'W'),
              (10, 10, 2, 'W')
          )
        SELECT
          ts, dur, cnt,
          CAST(sum_value AS INT64) AS sum_v,
          CAST(min_value AS INT64) AS min_v,
          CAST(max_value AS INT64) AS max_v,
          k0
        FROM _interval_self_intersect_agg!(data, v, (k0))
        ORDER BY k0 ASC, ts ASC;
        """,
        out=Csv("""
        "ts","dur","cnt","sum_v","min_v","max_v","k0"
        0,20,1,5,5,5,"T"
        20,0,0,0,"[NULL]","[NULL]","T"
        0,10,1,1,1,1,"W"
        10,10,1,2,2,2,"W"
        20,0,0,0,"[NULL]","[NULL]","W"
        """))

  def test_self_intersect_count_global(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        WITH
          data(ts, dur) AS (
            VALUES
              (0, 10),
              (5, 10)
          )
        SELECT ts, dur, group_id, cnt
        FROM _interval_self_intersect_count!(data, ())
        ORDER BY ts ASC;
        """,
        out=Csv("""
        "ts","dur","group_id","cnt"
        0,5,1,1
        5,5,2,2
        10,5,3,1
        15,0,4,0
        """))

  def test_self_intersect_count_empty_input(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        SELECT ts, dur, cnt, k0
        FROM _interval_self_intersect_count!(
          (SELECT 0 AS ts, 0 AS dur, '' AS k0 WHERE FALSE), (k0));
        """,
        out=Csv("""
        "ts","dur","cnt","k0"
        """))
