#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class IntervalsSelfIntersect(TestSuite):

  def test_simple_self_intersect_count(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        #      0 1 2 3 4 5 6 7 8
        # A:   - - - - _ _ _ _ _
        # B:   _ _ - - - - _ _ _
        # C:   _ _ _ _ - - - - _
        #
        # Overlaps:
        # [0,2): 1 interval (A)
        # [2,4): 2 intervals (A,B)
        # [4,6): 2 intervals (B,C)
        # [6,7): 1 interval (C)
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        CREATE PERFETTO TABLE intervals AS
          WITH data(id, ts, dur) AS (
            VALUES
            (0, 0, 4),
            (1, 2, 4),
            (2, 4, 3)
          )
          SELECT * FROM data;

        SELECT ts, dur, group_id, count
        FROM interval_to_table!(
          interval_intersect!(
            interval_partition!(intervals, (), ()),
            (interval_agg!(count, COUNT))
          ),
          (ts, dur, group_id, count)
        )
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","group_id","count"
        0,2,0,1
        2,2,1,2
        4,2,2,2
        6,1,3,1
        """))

  def test_self_intersect_sum_aggregation(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        CREATE PERFETTO TABLE intervals AS
          WITH data(id, ts, dur, value) AS (
            VALUES
            (0, 0, 4, 10),
            (1, 2, 4, 20),
            (2, 4, 3, 30)
          )
          SELECT * FROM data;

        SELECT
          ts,
          dur,
          group_id,
          count,
          sum_value AS sum
        FROM interval_to_table!(
          interval_intersect!(
            interval_partition!(intervals, (), (value)),
            (interval_agg!(count, COUNT), interval_agg!(value, SUM))
          ),
          (ts, dur, group_id, count, sum_value)
        )
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","group_id","count","sum"
        0,2,0,1,10.000000
        2,2,1,2,30.000000
        4,2,2,2,50.000000
        6,1,3,1,30.000000
        """))

  def test_self_intersect_max_aggregation(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        CREATE PERFETTO TABLE intervals AS
          WITH data(id, ts, dur, priority) AS (
            VALUES
            (0, 0, 4, 1),
            (1, 2, 4, 2),
            (2, 4, 3, 3)
          )
          SELECT * FROM data;

        SELECT
          ts,
          dur,
          group_id,
          count,
          max_priority AS max
        FROM interval_to_table!(
          interval_intersect!(
            interval_partition!(intervals, (), (priority)),
            (interval_agg!(count, COUNT), interval_agg!(priority, MAX))
          ),
          (ts, dur, group_id, count, max_priority)
        )
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","group_id","count","max"
        0,2,0,1,1.000000
        2,2,1,2,2.000000
        4,2,2,2,3.000000
        6,1,3,1,3.000000
        """))

  def test_self_intersect_min_aggregation(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        CREATE PERFETTO TABLE intervals AS
          WITH data(id, ts, dur, value) AS (
            VALUES
            (0, 0, 4, 10),
            (1, 2, 4, 20),
            (2, 4, 3, 30)
          )
          SELECT * FROM data;

        SELECT
          ts,
          dur,
          group_id,
          count,
          min_value AS min
        FROM interval_to_table!(
          interval_intersect!(
            interval_partition!(intervals, (), (value)),
            (interval_agg!(count, COUNT), interval_agg!(value, MIN))
          ),
          (ts, dur, group_id, count, min_value)
        )
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","group_id","count","min"
        0,2,0,1,10.000000
        2,2,1,2,10.000000
        4,2,2,2,20.000000
        6,1,3,1,30.000000
        """))

  def test_self_intersect_no_overlap(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        # A:   - _ _ _
        # B:   _ - _ _
        # C:   _ _ - _
        # No overlaps
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        CREATE PERFETTO TABLE intervals AS
          WITH data(id, ts, dur) AS (
            VALUES
            (0, 0, 1),
            (1, 1, 1),
            (2, 2, 1)
          )
          SELECT * FROM data;

        SELECT ts, dur, group_id, count
        FROM interval_to_table!(
          interval_intersect!(
            interval_partition!(intervals, (), ()),
            (interval_agg!(count, COUNT))
          ),
          (ts, dur, group_id, count)
        )
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","group_id","count"
        0,1,0,1
        1,1,1,1
        2,1,2,1
        """))

  def test_self_intersect_all_overlap(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        # All three intervals overlap in [2,5)
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        CREATE PERFETTO TABLE intervals AS
          WITH data(id, ts, dur, value) AS (
            VALUES
            (0, 0, 5, 100),
            (1, 1, 4, 200),
            (2, 2, 3, 300)
          )
          SELECT * FROM data;

        SELECT ts, dur, group_id, count, sum_value AS sum
        FROM interval_to_table!(
          interval_intersect!(
            interval_partition!(intervals, (), (value)),
            (interval_agg!(count, COUNT), interval_agg!(value, SUM))
          ),
          (ts, dur, group_id, count, sum_value)
        )
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","group_id","count","sum"
        0,1,0,1,100.000000
        1,1,1,2,300.000000
        2,3,2,3,600.000000
        5,0,3,2,300.000000
        """))

  def test_self_intersect_with_partitions(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        CREATE PERFETTO TABLE intervals AS
          WITH data(id, ts, dur, cpu, value) AS (
            VALUES
            (0, 0, 4, 0, 10),
            (1, 2, 4, 0, 20),
            (2, 0, 3, 1, 30),
            (3, 1, 3, 1, 40)
          )
          SELECT * FROM data;

        SELECT ts, dur, group_id, cpu, count, sum_value AS sum
        FROM interval_to_table!(
          interval_intersect!(
            interval_partition!(intervals, (cpu), (value)),
            (interval_agg!(count, COUNT), interval_agg!(value, SUM))
          ),
          (ts, dur, group_id, count, sum_value, cpu)
        )
        ORDER BY cpu, ts;
        """,
        out=Csv("""
        "ts","dur","group_id","cpu","count","sum"
        0,2,0,0,1,10.000000
        2,2,1,0,2,30.000000
        4,2,2,0,1,20.000000
        0,1,0,1,1,30.000000
        1,2,1,1,2,70.000000
        3,1,2,1,1,40.000000
        """))

  def test_self_intersect_avg_aggregation(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        CREATE PERFETTO TABLE intervals AS
          WITH data(id, ts, dur, value) AS (
            VALUES
            (0, 0, 3, 10),
            (1, 1, 3, 20),
            (2, 2, 3, 30)
          )
          SELECT * FROM data;

        SELECT ts, dur, group_id, count, avg_value AS avg
        FROM interval_to_table!(
          interval_intersect!(
            interval_partition!(intervals, (), (value)),
            (interval_agg!(count, COUNT), interval_agg!(value, AVG))
          ),
          (ts, dur, group_id, count, avg_value)
        )
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","group_id","count","avg"
        0,1,0,1,10.000000
        1,1,1,2,15.000000
        2,1,2,3,20.000000
        3,1,3,2,25.000000
        4,1,4,1,30.000000
        """))

  def test_self_intersect_no_aggregation(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        # Test unaggregated mode - just raw intersection buckets
        query="""
        INCLUDE PERFETTO MODULE intervals.self_intersect;

        CREATE PERFETTO TABLE intervals AS
          WITH data(id, ts, dur) AS (
            VALUES
            (0, 0, 4),
            (1, 2, 4),
            (2, 4, 3)
          )
          SELECT * FROM data;

        SELECT ts, dur, group_id
        FROM interval_to_table!(
          interval_intersect!(
            interval_partition!(intervals, (), ()),
            ()
          ),
          (ts, dur, group_id)
        )
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","group_id"
        0,2,0
        2,2,1
        4,2,2
        6,1,3
        """))
