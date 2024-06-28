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

  def test_intervals_intersect_easy(self):
    return DiffTestBlueprint(
        trace=DataPath("example_android_trace_30s.pb"),
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

        SELECT * FROM _new_interval_intersect!(A, B, ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1"
        1,1,0,0
        3,2,0,1
        6,1,0,2
        """))

  def test_compare_with_span_join_partitioned(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE big_foo AS
        SELECT
          ts,
          dur,
          id,
          cpu
        FROM sched
        WHERE dur > 0 AND utid != 0;

        CREATE PERFETTO TABLE small_foo AS
        SELECT
          ts + 1000 AS ts,
          dur + 1000 AS dur,
          id * 10 AS id,
          cpu
        FROM sched
        WHERE dur > 0 AND utid != 0;

        CREATE PERFETTO TABLE small_foo_for_sj AS
        SELECT
          id AS small_id,
          ts,
          dur,
          cpu
        FROM small_foo;

        CREATE PERFETTO TABLE big_foo_for_sj AS
        SELECT
          id AS big_id,
          ts,
          dur,
          cpu
        FROM big_foo;

        CREATE VIRTUAL TABLE sj_res
        USING SPAN_JOIN(
          small_foo_for_sj PARTITIONED cpu,
          big_foo_for_sj PARTITIONED cpu);

        CREATE PERFETTO TABLE both AS
        SELECT
          left_id,
          right_id,
          cat,
          count() AS c
        FROM (
          SELECT id_0 AS left_id, id_1 AS right_id, ts, dur, "ii" AS cat
          FROM _new_interval_intersect!(big_foo, small_foo, (cpu))
          UNION
          SELECT big_id AS left_id, small_id AS right_id, ts, dur, "sj" AS cat FROM sj_res
        )
          GROUP BY left_id, right_id;

        SELECT
          SUM(c) FILTER (WHERE c == 2) AS good,
          SUM(c) FILTER (WHERE c != 2) AS bad
        FROM both;
        """,
        out=Csv("""
          "good","bad"
          880364,"[NULL]"
        """))

  def test_compare_with_span_join_partitioned(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE big_foo AS
        SELECT
          ts,
          dur,
          id
        FROM sched
        WHERE dur > 0 AND utid == 44;

        CREATE PERFETTO TABLE small_foo AS
        SELECT
          ts,
          dur,
          id
        FROM sched
        WHERE dur > 0 AND utid == 103;

        CREATE PERFETTO TABLE small_foo_for_sj AS
        SELECT
          id AS small_id,
          ts,
          dur
        FROM small_foo;

        CREATE PERFETTO TABLE big_foo_for_sj AS
        SELECT
          id AS big_id,
          ts,
          dur
        FROM big_foo;

        CREATE VIRTUAL TABLE sj_res
        USING SPAN_JOIN(
          small_foo_for_sj,
          big_foo_for_sj);

        CREATE PERFETTO TABLE both AS
        SELECT
          left_id,
          right_id,
          cat,
          count() AS c
        FROM (
          SELECT id_0 AS left_id, id_1 AS right_id, ts, dur, "ii" AS cat
          FROM _new_interval_intersect!(big_foo, small_foo, ())
          UNION
          SELECT big_id AS left_id, small_id AS right_id, ts, dur, "sj" AS cat FROM sj_res
        )
          GROUP BY left_id, right_id;

        SELECT
          SUM(c) FILTER (WHERE c == 2) AS good,
          SUM(c) FILTER (WHERE c != 2) AS bad
        FROM both;
        """,
        out=Csv("""
          "good","bad"
          28,"[NULL]"
        """))

  def test_simple_interval_intersect_rev(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE A AS
          WITH data(id, ts, dur, c0, c1) AS (
            VALUES
            (0, 1, 6, 10, 3)
          )
          SELECT * FROM data;

        CREATE PERFETTO TABLE B AS
          WITH data(id, ts, dur, c0, c2) AS (
            VALUES
            (0, 0, 2, 10, 100),
            (1, 3, 2, 10, 200),
            (2, 6, 2, 20, 300)
          )
          SELECT * FROM data;

        SELECT id_0, id_1
        FROM _new_interval_intersect!(A, B, (c0))
        ORDER BY 1, 2;
        """,
        out=Csv("""
        "id_0","id_1"
        0,0
        0,1
        """))

  def test_ii_wrong_partition(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE A
        AS
        WITH x(id, ts, dur, c0) AS (VALUES(1, 1, 1, 1), (2, 3, 1, 2))
        SELECT * FROM x;

        CREATE PERFETTO TABLE B
        AS
        WITH x(id, ts, dur, c0) AS (VALUES(1, 5, 1, 3))
        SELECT * FROM x;

        SELECT ts FROM _new_interval_intersect!(A, B, (c0));
        """,
        out=Csv("""
        "ts"
        """))
