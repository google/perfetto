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

  def test_simple_ii_operator(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""

        CREATE PERFETTO TABLE A AS
          WITH data(id, ts, ts_end, c0, c1) AS (
            VALUES
            (0, 1, 7, 10, 3)
          )
          SELECT * FROM data;

        CREATE PERFETTO TABLE B AS
          WITH data(id, ts, ts_end, c0, c2) AS (
            VALUES
            (0, 0, 2, 10, 100),
            (1, 3, 5, 10, 200),
            (2, 6, 8, 20, 300)
          )
          SELECT * FROM data;

        SELECT a.id AS a_id, b.id AS b_id
        FROM __intrinsic_ii_with_interval_tree('A', 'c0, c1') a
        JOIN __intrinsic_ii_with_interval_tree('B', 'c0, c2') b
        WHERE a.ts < b.ts_end AND a.ts_end > b.ts
        """,
        out=Csv("""
        "a_id","b_id"
        0,1
        0,0
        0,2
        """))

  def test_ii_operator_big(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO TABLE big_foo AS
        SELECT
          id,
          ts,
          ts+dur AS ts_end
        FROM sched
        WHERE dur != -1
        ORDER BY ts;

        CREATE PERFETTO TABLE small_foo AS
        SELECT
        id * 10 AS id,
        ts + 1000 AS ts,
        ts_end + 1000 AS ts_end
        FROM big_foo
        LIMIT 10
        OFFSET 5;

        CREATE PERFETTO TABLE res AS
        SELECT a.id AS a_id, b.id AS b_id
        FROM __intrinsic_ii_with_interval_tree('small_foo', '') a
        JOIN __intrinsic_ii_with_interval_tree('big_foo', '') b
        WHERE a.ts < b.ts_end AND a.ts_end > b.ts;

        SELECT * FROM res
        ORDER BY a_id, b_id
        LIMIT 10;
        """,
        out=Csv("""
        "a_id","b_id"
        50,1
        50,5
        50,6
        60,1
        60,6
        60,7
        60,8
        70,1
        70,6
        70,7
        """))

  def test_ii_with_ii_operator(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE big_foo AS
        SELECT
          ts,
          ts + dur as ts_end,
          id * 10 AS id
        FROM sched
        WHERE utid == 1 AND dur > 0;

        CREATE PERFETTO TABLE small_foo AS
        SELECT
        ts + 1000 AS ts,
        ts + dur + 1000 AS ts_end,
        id
        FROM sched
        WHERE utid == 1 AND dur > 0;

        CREATE PERFETTO TABLE small_foo_for_ii AS
        SELECT id, ts, ts_end - ts AS dur
        FROM small_foo;

        CREATE PERFETTO TABLE big_foo_for_ii AS
        SELECT id, ts, ts_end - ts AS dur
        FROM big_foo;

        CREATE PERFETTO TABLE both AS
        SELECT
          id_0,
          id_1,
          cat,
          count() AS c,
          MAX(ts) AS max_ts, MAX(dur) AS max_dur
        FROM (
          SELECT a.id AS id_0, b.id AS id_1, 0 AS ts, 0 AS dur, "it" AS cat
          FROM __intrinsic_ii_with_interval_tree('big_foo', '') a
          JOIN __intrinsic_ii_with_interval_tree('small_foo', '') b
          WHERE a.ts < b.ts_end AND a.ts_end > b.ts
          UNION
          SELECT id_0, id_1, ts, dur, "ii" AS cat
          FROM _interval_intersect!(big_foo_for_ii, small_foo_for_ii, ())
          WHERE dur != 0
        )
          GROUP BY id_0, id_1;

        SELECT
          SUM(c) FILTER (WHERE c == 2) AS good,
          SUM(c) FILTER (WHERE c != 2) AS bad
        FROM both;
        """,
        out=Csv("""
          "good","bad"
          314,"[NULL]"
        """))

  def test_ii_operator_partitioned_big(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE big_foo AS
        SELECT
          ts,
          ts + dur as ts_end,
          id * 10 AS id,
          cpu AS c0
        FROM sched
        WHERE dur != -1;

        CREATE PERFETTO TABLE small_foo AS
        SELECT
          ts + 1000 AS ts,
          ts + dur + 1000 AS ts_end,
          id,
          cpu AS c0
        FROM sched
        WHERE dur != -1;

        CREATE PERFETTO TABLE res AS
        SELECT a.id AS a_id, b.id AS b_id
        FROM __intrinsic_ii_with_interval_tree('small_foo', 'c0') a
        JOIN __intrinsic_ii_with_interval_tree('big_foo', 'c0') b
        USING (c0)
        WHERE a.ts < b.ts_end AND a.ts_end > b.ts;

        SELECT * FROM res
        ORDER BY a_id, b_id
        LIMIT 10;
        """,
        out=Csv("""
        "a_id","b_id"
        0,0
        0,10
        1,10
        1,430
        2,20
        2,30
        3,30
        3,40
        4,40
        4,50
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

        SELECT * FROM _interval_intersect!(A, B, ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1"
        1,1,0,0
        3,2,0,1
        6,1,0,2
        """))

  def test_compare_ii_operator_with_span_join(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE big_foo AS
        SELECT
          ts,
          ts + dur as ts_end,
          id * 10 AS id,
          cpu AS c0
        FROM sched
        WHERE dur != -1;

        CREATE PERFETTO TABLE small_foo AS
        SELECT
          ts + 1000 AS ts,
          ts + dur + 1000 AS ts_end,
          id,
          cpu AS c0
        FROM sched
        WHERE dur != -1;

        CREATE PERFETTO TABLE small_foo_for_sj AS
        SELECT
          id AS small_id,
          ts,
          ts_end - ts AS dur,
          c0
        FROM small_foo
        WHERE dur != 0;

        CREATE PERFETTO TABLE big_foo_for_sj AS
        SELECT
          id AS big_id,
          ts,
          ts_end - ts AS dur,
          c0
        FROM big_foo
        WHERE dur != 0;

        CREATE VIRTUAL TABLE sj_res
        USING SPAN_JOIN(
          small_foo_for_sj PARTITIONED c0,
          big_foo_for_sj PARTITIONED c0);

        CREATE PERFETTO TABLE both AS
        SELECT
          id_0,
          id_1,
          cat,
          count() AS c,
          MAX(ts) AS max_ts, MAX(dur) AS max_dur
        FROM (
          SELECT a.id AS id_0, b.id AS id_1, 0 AS ts, 0 AS dur, "it" AS cat
          FROM __intrinsic_ii_with_interval_tree('big_foo', 'c0') a
          JOIN __intrinsic_ii_with_interval_tree('small_foo', 'c0') b
          USING (c0)
          WHERE a.ts < b.ts_end AND a.ts_end > b.ts
          UNION
          SELECT big_id AS id_0, small_id AS id_1, ts, dur, "sj" AS cat FROM sj_res
        )
          GROUP BY id_0, id_1;

        SELECT
          SUM(c) FILTER (WHERE c == 2) AS good,
          SUM(c) FILTER (WHERE c != 2) AS bad
        FROM both;
        """,
        out=Csv("""
          "good","bad"
          1538288,"[NULL]"
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
          id_0,
          id_1,
          cpu,
          cat,
          count() AS c
        FROM (
          SELECT id_0, id_1, ts, dur, cpu, "ii" AS cat
          FROM _interval_intersect!(big_foo, small_foo, (cpu))
          UNION
          SELECT big_id AS id_0, small_id AS id_1, ts, dur, cpu, "sj" AS cat FROM sj_res
        )
          GROUP BY id_0, id_1, ts, dur, cpu ;

        SELECT
          SUM(c) FILTER (WHERE c == 2) AS good,
          SUM(c) FILTER (WHERE c != 2) AS bad
        FROM both;
        """,
        out=Csv("""
          "good","bad"
          880364,"[NULL]"
        """))

  def test_compare_ii_with_span_join(self):
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
          FROM _interval_intersect!(big_foo, small_foo, ())
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

  def test_simple_interval_intersect(self):
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

        SELECT id_0, id_1, c0
        FROM _interval_intersect!(A, B, (c0))
        ORDER BY 1, 2;
        """,
        out=Csv("""
        "id_0","id_1","c0"
        0,0,10
        0,1,10
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

        SELECT ts FROM _interval_intersect!(A, B, (c0));
        """,
        out=Csv("""
        "ts"
        """))

  def test_ii_operator_wrong_partition(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        CREATE PERFETTO TABLE A
        AS
        WITH x(id, ts, ts_end, c0) AS (VALUES(1, 1, 2, 1), (2, 3, 4, 2))
        SELECT * FROM x;

        CREATE PERFETTO TABLE B
        AS
        WITH x(id, ts, ts_end, c0) AS (VALUES(1, 5, 6, 3))
        SELECT * FROM x;

        SELECT
        a.id AS a_id,
        b.id AS b_id
        FROM __intrinsic_ii_with_interval_tree('A', 'c0') a
        JOIN __intrinsic_ii_with_interval_tree('B', 'c0') b
        USING (c0)
        WHERE a.ts < b.ts_end AND a.ts_end > b.ts;
        """,
        out=Csv("""
        "a_id","b_id"
        """))
