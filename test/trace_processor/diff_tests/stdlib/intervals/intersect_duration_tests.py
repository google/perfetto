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

from python.generators.diff_tests.testing import TextProto
from python.generators.diff_tests.testing import Csv, ExpectedError
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite

# What an interval intersect does with the durations which are not a plain
# span: a duration of zero, which is a point, and a negative duration, which
# a caller has not filtered out. Points are also called "instants" in
# interval_tree.h.
#
# An interval is [ts, ts + dur), so its end is outside it: an interval and a
# point meet when the point lies in that range, and two intervals meet when
# they share an instant. Each test below states which instants are shared.


class IntervalsIntersectDurations(TestSuite):

  # A point meets an interval which covers the instant it sits at. The
  # interval's own start is such an instant and its end is not.
  def test_point_against_interval_positions(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        #        5    10   15   19   20   25
        # O:          [--------------)
        # P:     .    .    .    .    .    .
        # res:        x    x    x
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE o AS
          WITH data(id, ts, dur) AS (VALUES (0, 10, 10))
          SELECT * FROM data;

        CREATE PERFETTO TABLE p AS
          WITH data(id, ts, dur) AS (
            VALUES (0, 5, 0), (1, 10, 0), (2, 15, 0), (3, 19, 0), (4, 20, 0),
                   (5, 25, 0)
          )
          SELECT * FROM data;

        SELECT ts, dur, id_0 AS point_id, id_1 AS interval_id
        FROM _interval_intersect!((p, o), ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","point_id","interval_id"
        10,0,1,0
        15,0,2,0
        19,0,3,0
        """))

  # The same meetings, with the roles of the two tables swapped: which table
  # holds the points does not change which instants are shared.
  def test_interval_against_point_positions(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE o AS
          WITH data(id, ts, dur) AS (VALUES (0, 10, 10))
          SELECT * FROM data;

        CREATE PERFETTO TABLE p AS
          WITH data(id, ts, dur) AS (
            VALUES (0, 5, 0), (1, 10, 0), (2, 15, 0), (3, 19, 0), (4, 20, 0),
                   (5, 25, 0)
          )
          SELECT * FROM data;

        SELECT ts, dur, id_0 AS interval_id, id_1 AS point_id
        FROM _interval_intersect!((o, p), ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","interval_id","point_id"
        10,0,0,1
        15,0,0,2
        19,0,0,3
        """))

  # Two points meet only where they sit at the same instant.
  def test_point_against_point(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE a AS
          WITH data(id, ts, dur) AS (VALUES (0, 10, 0), (1, 12, 0))
          SELECT * FROM data;

        CREATE PERFETTO TABLE b AS
          WITH data(id, ts, dur) AS (VALUES (0, 10, 0), (1, 11, 0))
          SELECT * FROM data;

        SELECT ts, dur, id_0, id_1
        FROM _interval_intersect!((a, b), ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1"
        10,0,0,0
        """))

  # Two intervals meet over the instants they share, so touching end to start
  # is no meeting at all.
  def test_interval_against_interval_positions(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        #     0         10        20        30
        # O:            [---------)
        # P0: [----)                          before
        # P1: [---------)                     up to the start
        # P2:      [---------)                over the start
        # P3:             [-----)             inside
        # P4: [-----------------------------) over the whole
        # P5:           [---------)           the same
        # P6:                [---------)      over the end
        # P7:                     [---------) from the end
        # P8:                          [----) after
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE o AS
          WITH data(id, ts, dur) AS (VALUES (0, 10, 10))
          SELECT * FROM data;

        CREATE PERFETTO TABLE p AS
          WITH data(id, ts, dur) AS (
            VALUES (0, 0, 5), (1, 0, 10), (2, 5, 10), (3, 12, 6), (4, 0, 30),
                   (5, 10, 10), (6, 15, 10), (7, 20, 10), (8, 25, 5)
          )
          SELECT * FROM data;

        SELECT id_0 AS probe_id, ts, dur
        FROM _interval_intersect!((p, o), ())
        ORDER BY probe_id;
        """,
        out=Csv("""
        "probe_id","ts","dur"
        2,10,5
        3,12,6
        4,10,10
        5,10,10
        6,15,5
        """))

  # A point at the join of two intervals belongs to the one it starts.
  def test_point_at_a_boundary_between_intervals(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE o AS
          WITH data(id, ts, dur) AS (VALUES (0, 0, 10), (1, 10, 10))
          SELECT * FROM data;

        CREATE PERFETTO TABLE p AS
          WITH data(id, ts, dur) AS (VALUES (0, 10, 0))
          SELECT * FROM data;

        SELECT ts, dur, id_0 AS point_id, id_1 AS interval_id
        FROM _interval_intersect!((p, o), ())
        ORDER BY interval_id;
        """,
        out=Csv("""
        "ts","dur","point_id","interval_id"
        10,0,0,1
        """))

  # A point meets every interval of an overlapping set which covers it.
  def test_point_against_overlapping_intervals(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE o AS
          WITH data(id, ts, dur) AS (
            VALUES (0, 0, 30), (1, 5, 20), (2, 10, 2), (3, 18, 10)
          )
          SELECT * FROM data;

        CREATE PERFETTO TABLE p AS
          WITH data(id, ts, dur) AS (VALUES (0, 20, 0))
          SELECT * FROM data;

        SELECT ts, dur, id_1 AS interval_id
        FROM _interval_intersect!((p, o), ())
        ORDER BY interval_id;
        """,
        out=Csv("""
        "ts","dur","interval_id"
        20,0,0
        20,0,1
        20,0,3
        """))

  # A point which survives two rounds of narrowing stays a point, and the
  # result carries the point rather than either interval's bounds.
  def test_point_through_three_tables(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE a AS
          WITH data(id, ts, dur) AS (VALUES (0, 0, 100))
          SELECT * FROM data;

        CREATE PERFETTO TABLE b AS
          WITH data(id, ts, dur) AS (VALUES (0, 10, 40))
          SELECT * FROM data;

        CREATE PERFETTO TABLE c AS
          WITH data(id, ts, dur) AS (VALUES (0, 20, 0), (1, 60, 0))
          SELECT * FROM data;

        SELECT ts, dur, id_0, id_1, id_2
        FROM _interval_intersect!((a, b, c), ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1","id_2"
        20,0,0,0,0
        """))

  # Narrowing keeps only the instants every table shares, so a point outside
  # any one of them leaves nothing.
  def test_point_outside_one_of_three_tables(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE a AS
          WITH data(id, ts, dur) AS (VALUES (0, 0, 100))
          SELECT * FROM data;

        CREATE PERFETTO TABLE b AS
          WITH data(id, ts, dur) AS (VALUES (0, 10, 5))
          SELECT * FROM data;

        CREATE PERFETTO TABLE c AS
          WITH data(id, ts, dur) AS (VALUES (0, 20, 0))
          SELECT * FROM data;

        SELECT ts, dur
        FROM _interval_intersect!((a, b, c), ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur"
        """))

  # Narrowing an interval down to a point keeps the point: the shared instants
  # of [0, 100) and [20, 30) and the point 25 are the point.
  def test_interval_narrowed_to_a_point(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE a AS
          WITH data(id, ts, dur) AS (VALUES (0, 0, 100))
          SELECT * FROM data;

        CREATE PERFETTO TABLE b AS
          WITH data(id, ts, dur) AS (VALUES (0, 20, 10))
          SELECT * FROM data;

        CREATE PERFETTO TABLE c AS
          WITH data(id, ts, dur) AS (VALUES (0, 25, 0))
          SELECT * FROM data;

        SELECT ts, dur, id_0, id_1, id_2
        FROM _interval_intersect!((a, b, c), ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1","id_2"
        25,0,0,0,0
        """))

  # Points meet across all three tables only at one shared instant.
  def test_coincident_points_through_three_tables(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE a AS
          WITH data(id, ts, dur) AS (VALUES (0, 7, 0), (1, 9, 0))
          SELECT * FROM data;

        CREATE PERFETTO TABLE b AS
          WITH data(id, ts, dur) AS (VALUES (0, 7, 0), (1, 8, 0))
          SELECT * FROM data;

        CREATE PERFETTO TABLE c AS
          WITH data(id, ts, dur) AS (VALUES (0, 0, 10))
          SELECT * FROM data;

        SELECT ts, dur, id_0, id_1, id_2
        FROM _interval_intersect!((a, b, c), ())
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1","id_2"
        7,0,0,0,0
        """))

  # Points are kept apart by their partition like any other interval.
  def test_points_in_partitions(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE a AS
          WITH data(id, ts, dur, c0) AS (
            VALUES (0, 10, 0, 1), (1, 10, 0, 2)
          )
          SELECT * FROM data;

        CREATE PERFETTO TABLE b AS
          WITH data(id, ts, dur, c0) AS (
            VALUES (0, 0, 20, 2), (1, 0, 20, 3)
          )
          SELECT * FROM data;

        SELECT ts, dur, id_0, id_1, c0
        FROM _interval_intersect!((a, b), (c0))
        ORDER BY c0;
        """,
        out=Csv("""
        "ts","dur","id_0","id_1","c0"
        10,0,1,0,2
        """))

  # A duration which never ends is rejected rather than read as a span: a
  # caller which has not filtered one out is told so.
  def test_negative_duration_is_rejected(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE a AS
          WITH data(id, ts, dur) AS (VALUES (0, 0, 10))
          SELECT * FROM data;

        CREATE PERFETTO TABLE b AS
          WITH data(id, ts, dur) AS (VALUES (0, 5, -1))
          SELECT * FROM data;

        SELECT ts, dur FROM _interval_intersect!((a, b), ());
        """,
        out=ExpectedError(
            "Interval intersect only works on intervals with non negative "
            "duration."))

  # The table a duration which never ends sits in does not matter.
  def test_negative_duration_in_the_first_table_is_rejected(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE a AS
          WITH data(id, ts, dur) AS (VALUES (0, 0, -1))
          SELECT * FROM data;

        CREATE PERFETTO TABLE b AS
          WITH data(id, ts, dur) AS (VALUES (0, 5, 10))
          SELECT * FROM data;

        SELECT ts, dur FROM _interval_intersect!((a, b), ());
        """,
        out=ExpectedError(
            "Interval intersect only works on intervals with non negative "
            "duration."))

  # A duration which never ends is rejected even where no other interval
  # could have met it.
  def test_negative_duration_is_rejected_even_when_alone(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE a AS
          WITH data(id, ts, dur) AS (VALUES (0, 0, 10))
          SELECT * FROM data;

        CREATE PERFETTO TABLE b AS
          WITH data(id, ts, dur) AS (VALUES (0, 500, -1))
          SELECT * FROM data;

        SELECT ts, dur FROM _interval_intersect!((a, b), ());
        """,
        out=ExpectedError(
            "Interval intersect only works on intervals with non negative "
            "duration."))
