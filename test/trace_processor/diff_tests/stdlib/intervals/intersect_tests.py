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

  def test_simple_interval_intersect(self):
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

        SELECT ts, dur, id_0, id_1
        FROM _interval_intersect!((A, B), ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1"
        1,1,0,0
        3,2,0,1
        6,1,0,2
        """))

  def test_simple_interval_intersect_two_tabs(self):
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

        SELECT ts, dur, id_0, id_1
        FROM _interval_intersect!((B, A), ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1"
        1,1,0,0
        3,2,1,0
        6,1,2,0
        """))

  def test_simple_interval_intersect_three_tabs(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        #      0 1 2 3 4 5 6 7
        # A:   0 1 1 1 1 1 1 0
        # B:   1 1 0 1 1 0 1 1
        # C:   1 0 1 1 1 1 0 1
        # res: 0 0 0 1 1 0 0 0
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

        CREATE PERFETTO TABLE C AS
          WITH data(id, ts, dur) AS (
            VALUES
            (10, 0, 1),
            (20, 2, 4),
            (30, 7, 1)
          )
          SELECT * FROM data;

        SELECT ts, dur, id_0, id_1, id_2
        FROM _interval_intersect!((A, B, C), ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1","id_2"
        3,2,0,1,20
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

        SELECT ts, dur, id_0, id_1
        FROM _interval_intersect!((A, B), ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1"
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

        SELECT ts, dur, id_0, id_1
        FROM _interval_intersect!((B, A), ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1"
        """))

  def test_no_empty(self):
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
        SELECT * FROM A LIMIT 0;

        SELECT ts, dur, id_0, id_1
        FROM _interval_intersect!((A, B), ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1"
        """))

  def test_no_empty_rev(self):
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
        SELECT * FROM A LIMIT 0;

        SELECT ts, dur, id_0, id_1
        FROM _interval_intersect!((B, A), ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1"
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

        SELECT ts, dur, id_0, id_1
        FROM _interval_intersect!((A, B), ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1"
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

        SELECT ts, dur, id_0, id_1
        FROM _interval_intersect!((B, A), ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1"
        """))

  def test_single_interval(self):
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

        SELECT *
        FROM _interval_intersect_single!(1, 6, B)
        ORDER BY ts;
        """,
        out=Csv("""
        "id","ts","dur"
        0,1,1
        1,3,2
        2,6,1
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

        SELECT ts FROM _interval_intersect!((A, B), (c0));
        """,
        out=Csv("""
        "ts"
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
          FROM _interval_intersect!((big_foo, small_foo), (cpu))
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
          FROM _interval_intersect!((big_foo, small_foo), ())
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

  def test_sanity_check(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE trace_interval AS
        SELECT
          0 AS id,
          TRACE_START() AS ts,
          TRACE_DUR() AS dur;

        CREATE PERFETTO TABLE non_overlapping AS
        SELECT
          id, ts, dur
        FROM thread_state
        WHERE utid = 1 AND dur != -1;

        WITH ii AS (
          SELECT *
          FROM _interval_intersect!((trace_interval, non_overlapping), ())
        )
        SELECT
          (SELECT count(*) FROM ii) AS ii_count,
          (SELECT count(*) FROM non_overlapping) AS thread_count,
          (SELECT sum(dur) FROM ii) AS ii_sum,
          (SELECT sum(dur) FROM non_overlapping) AS thread_sum;
        """,
        out=Csv("""
        "ii_count","thread_count","ii_sum","thread_sum"
        313,313,27540674879,27540674879
        """))

  def test_sanity_check_single_interval(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE non_overlapping AS
        SELECT
          id, ts, dur
        FROM thread_state
        WHERE utid = 1 AND dur != -1;

        WITH ii AS (
          SELECT *
          FROM _interval_intersect_single!(
            TRACE_START(),
            TRACE_DUR(),
            non_overlapping)
        )
        SELECT
          (SELECT count(*) FROM ii) AS ii_count,
          (SELECT count(*) FROM non_overlapping) AS thread_count,
          (SELECT sum(dur) FROM ii) AS ii_sum,
          (SELECT sum(dur) FROM non_overlapping) AS thread_sum;
        """,
        out=Csv("""
        "ii_count","thread_count","ii_sum","thread_sum"
        313,313,27540674879,27540674879
        """))

  def test_sanity_multiple_partitions(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        SELECT * FROM _interval_intersect!(
          ((SELECT id, ts, dur, utid, cpu FROM sched WHERE dur > 0 LIMIT 10),
          (SELECT id, ts, dur, utid, cpu FROM sched WHERE dur > 0 LIMIT 10)),
          (utid, cpu)
        );
        """,
        out=Csv("""
        "ts","dur","id_0","id_1","utid","cpu"
        70730062200,125364,0,0,1,0
        70730187564,20297242,1,1,0,0
        70731483398,24583,9,9,10,3
        70731458606,24792,8,8,9,3
        70731393294,42396,5,5,6,3
        70731435690,22916,6,6,7,3
        70731161731,35000,3,3,4,3
        70731196731,196563,4,4,5,3
        70731438502,55261,7,7,8,6
        70731135898,25833,2,2,2,3
        """))

  def test_sanity_single_partitions(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        SELECT * FROM _interval_intersect!(
          ((SELECT id, ts, dur, utid, cpu FROM sched WHERE dur > 0 LIMIT 10),
          (SELECT id, ts, dur, utid, cpu FROM sched WHERE dur > 0 LIMIT 10)),
          (utid)
        );
        """,
        out=Csv("""
        "ts","dur","id_0","id_1","utid"
        70731458606,24792,8,8,9
        70731161731,35000,3,3,4
        70731393294,42396,5,5,6
        70730187564,20297242,1,1,0
        70731135898,25833,2,2,2
        70731438502,55261,7,7,8
        70731483398,24583,9,9,10
        70731196731,196563,4,4,5
        70731435690,22916,6,6,7
        70730062200,125364,0,0,1
        """))

  def test_sanity_multiple_tables_and_partitions(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        SELECT * FROM _interval_intersect!(
          (
            (SELECT id, ts, dur, utid, cpu FROM sched WHERE dur > 0 LIMIT 10),
            (SELECT id, ts, dur, utid, cpu FROM sched WHERE dur > 0 LIMIT 10),
            (SELECT id, ts, dur, utid, cpu FROM sched WHERE dur > 0 LIMIT 10)
          ),
          (utid, cpu)
        );
        """,
        out=Csv("""
        "ts","dur","id_0","id_1","id_2","utid","cpu"
        70730062200,125364,0,0,0,1,0
        70730187564,20297242,1,1,1,0,0
        70731483398,24583,9,9,9,10,3
        70731458606,24792,8,8,8,9,3
        70731393294,42396,5,5,5,6,3
        70731435690,22916,6,6,6,7,3
        70731161731,35000,3,3,3,4,3
        70731196731,196563,4,4,4,5,3
        70731438502,55261,7,7,7,8,6
        70731135898,25833,2,2,2,2,3
        """))

  def test_sanity_multiple_tables_and_partitions(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        SELECT * FROM _interval_intersect!(
          (
            (SELECT id, ts, dur, utid, cpu FROM sched WHERE dur > 0 LIMIT 10),
            (SELECT id, ts, dur, utid, cpu FROM sched WHERE dur > 0 LIMIT 10),
            (SELECT id, ts, dur, utid, cpu FROM sched WHERE dur > 0 LIMIT 10)
          ),
          (utid, cpu)
        );
        """,
        out=Csv("""
        "ts","dur","id_0","id_1","id_2","utid","cpu"
        70730062200,125364,0,0,0,1,0
        70730187564,20297242,1,1,1,0,0
        70731483398,24583,9,9,9,10,3
        70731458606,24792,8,8,8,9,3
        70731393294,42396,5,5,5,6,3
        70731435690,22916,6,6,6,7,3
        70731161731,35000,3,3,3,4,3
        70731196731,196563,4,4,4,5,3
        70731438502,55261,7,7,7,8,6
        70731135898,25833,2,2,2,2,3
        """))

  def test_multiple_tables_against_ii(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE foo AS
        SELECT id, ts, dur FROM sched
        WHERE dur > 0 AND cpu = 0
        ORDER BY ts;

        CREATE PERFETTO TABLE bar AS
        SELECT id, ts, dur FROM sched
        WHERE dur > 0 AND cpu = 1
        ORDER BY ts;

        CREATE PERFETTO TABLE baz AS
        SELECT id, ts, dur FROM sched
        WHERE dur > 0 AND cpu = 2
        ORDER BY ts;

        CREATE PERFETTO TABLE ii_foo_and_bar AS
        SELECT
          ROW_NUMBER() OVER (ORDER BY ts) AS id,
          ts, dur, id_0 AS id_foo, id_1 AS id_bar
        FROM _interval_intersect!((foo, bar), ())
        ORDER BY ts;

        CREATE PERFETTO TABLE ii_foo_bar_baz AS
        SELECT id_foo, id_bar, id_1 AS id_baz, ii.ts, ii.dur
        FROM _interval_intersect!((ii_foo_and_bar, baz), ()) ii
        JOIN ii_foo_and_bar ON ii_foo_and_bar.id = ii.id_0;

        WITH unioned AS (
            SELECT id_foo, id_bar, id_baz, ts, dur, "std" AS cat
            FROM ii_foo_bar_baz
            UNION
            SELECT id_0 AS id_foo, id_1 AS id_bar, id_2 AS id_baz, ts, dur, "triple" AS cat
            FROM _interval_intersect!((foo, bar, baz), ())
        ),
        counted AS (
          SELECT *, count() c FROM unioned GROUP BY ts, dur, id_foo, id_bar, id_baz
        )
        SELECT
          SUM(c) FILTER (WHERE c == 2) AS good,
          SUM(c) FILTER (WHERE c != 2) AS bad
        FROM counted;
        """,
        out=Csv("""
        "good","bad"
        303178,"[NULL]"
        """))

  def test_multiple_tables_big(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE foo AS
        SELECT id, ts, dur FROM sched
        WHERE dur > 0 AND cpu = 0
        ORDER BY ts;

        CREATE PERFETTO TABLE bar AS
        SELECT id, ts, dur FROM sched
        WHERE dur > 0 AND cpu = 1
        ORDER BY ts;

        CREATE PERFETTO TABLE baz AS
        SELECT id, ts, dur FROM sched
        WHERE dur > 0 AND cpu = 2
        ORDER BY ts;

        SELECT * FROM _interval_intersect!((foo, bar, baz), ())
        ORDER BY ts
        LIMIT 10;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1","id_2"
        70799077155,1187135,44,133,132
        70800264290,473386,44,138,132
        70800737676,352500,44,139,132
        70801090176,643906,140,139,132
        70801734082,121615,141,139,132
        70801855697,68073,141,139,142
        70801923770,61354,141,139,143
        70801985124,5054323,141,139,144
        70807039447,65261,141,139,145
        70807104708,50572,141,139,146
        """))
