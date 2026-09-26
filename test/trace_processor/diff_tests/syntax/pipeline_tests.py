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

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv, ExpectedError, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class PerfettoPipeline(TestSuite):

  def test_tree_accumulate_up_and_down(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE tree AS
        SELECT 0 AS id, NULL AS parent_id, 'root' AS name, 10 AS self
        UNION ALL SELECT 1, 0, 'a', 20
        UNION ALL SELECT 2, 0, 'b', 30
        UNION ALL SELECT 3, 1, 'c', 40;

        CREATE PERFETTO TABLE totals AS
        FROM tree
        |> TREE ACCUMULATE UP SUM(self) AS total
        |> TREE ACCUMULATE DOWN SUM(self) AS path;

        SELECT name, self, total, path FROM totals ORDER BY name;
        """,
        out=Csv("""
        "name","self","total","path"
        "a",20,60,30
        "b",30,30,40
        "c",40,40,70
        "root",10,100,10
        """))

  # Must match the descendant closure, which this is meant to replace.
  def test_tree_accumulate_matches_closure(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE piped AS
        FROM (SELECT id, parent_id, dur FROM slice)
        |> TREE ACCUMULATE UP SUM(dur) AS subtree_dur
        |> TREE ACCUMULATE DOWN SUM(dur) AS path_dur;

        CREATE PERFETTO TABLE closure AS
        WITH RECURSIVE descendants(root_id, id) AS (
          SELECT id, id FROM slice
          UNION ALL
          SELECT d.root_id, c.id
          FROM descendants d JOIN slice c ON c.parent_id = d.id
        )
        SELECT
          d.root_id AS ancestor,
          d.id AS descendant,
          s.dur AS dur
        FROM descendants d JOIN slice s ON s.id = d.id;

        SELECT
          (SELECT count(*) FROM piped) AS rows,
          (
            SELECT count(*) FROM (
              SELECT id, subtree_dur, path_dur FROM piped
              EXCEPT
              SELECT
                up.id, up.subtree_dur, down.path_dur
              FROM (
                SELECT ancestor AS id, sum(dur) AS subtree_dur
                FROM closure GROUP BY ancestor
              ) up
              JOIN (
                SELECT c.descendant AS id, sum(s.dur) AS path_dur
                FROM closure c JOIN slice s ON s.id = c.ancestor
                GROUP BY c.descendant
              ) down USING (id)
            )
          ) AS mismatches;
        """,
        out=Csv("""
        "rows","mismatches"
        74228,0
        """))

  def test_pipeline_errors_name_the_problem(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        FROM (SELECT 1 AS id, NULL AS parent_id)
        |> TREE ACCUMULATE UP MAX(id) AS biggest;
        """,
        out=ExpectedError('aggregate MAX is not supported yet'))

  def test_a_pipeline_needs_the_pragma(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        FROM (SELECT 1 AS id, NULL AS parent_id)
        |> TREE ACCUMULATE UP SUM(id) AS total;
        """,
        out=ExpectedError('Pipelines are not enabled'))

  def test_the_pragma_admits_a_pipeline(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        FROM (SELECT 1 AS id, NULL AS parent_id, 7 AS self)
        |> TREE ACCUMULATE UP SUM(self) AS total;
        """,
        out=Csv("""
        "id","parent_id","self","total"
        1,"[NULL]",7,7
        """))

  def test_the_pragma_can_be_turned_back_off(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        PERFETTO PRAGMA pipelines = 0;
        FROM (SELECT 1 AS id, NULL AS parent_id)
        |> TREE ACCUMULATE UP SUM(id) AS total;
        """,
        out=ExpectedError('Pipelines are not enabled'))

  def test_an_unknown_pragma_says_so(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA nonsense = 1;
        """,
        out=ExpectedError("there is no setting 'nonsense'"))

  def test_a_pragma_takes_a_whole_number(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 'yes';
        """,
        out=ExpectedError('expected a whole number'))

  # The region carries its own bounds, and each operand's row comes along
  # under the name it was given.
  def test_interval_intersection_of_two_tables(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS
        SELECT 0 AS ts, 30 AS dur, 7 AS freq
        UNION ALL SELECT 40, 10, 8;

        CREATE PERFETTO TABLE b AS
        SELECT 10 AS ts, 10 AS dur, 1 AS state
        UNION ALL SELECT 35, 10, 2;

        INTERVAL INTERSECTION OF (a AS x, b AS y)
        |> SELECT ts, dur, x.freq, y.state;
        """,
        out=Csv("""
        "ts","dur","freq","state"
        10,10,7,1
        40,5,8,2
        """))

  # An operand's own columns are still readable through its name, so a caller
  # which needs the whole interval rather than the region can have it.
  def test_interval_intersection_keeps_the_operand_bounds(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 30 AS dur;
        CREATE PERFETTO TABLE b AS SELECT 10 AS ts, 100 AS dur;

        INTERVAL INTERSECTION OF (a AS x, b AS y)
        |> SELECT ts, dur, x.ts AS x_ts, x.dur AS x_dur, y.ts AS y_ts;
        """,
        out=Csv("""
        "ts","dur","x_ts","x_dur","y_ts"
        10,20,0,30,10
        """))

  # PER confines regions to rows which agree on the columns named, and those
  # columns come out under their bare names.
  def test_interval_intersection_per_a_key(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS
        SELECT 0 AS ts, 30 AS dur, 1 AS cpu
        UNION ALL SELECT 0, 30, 2;

        CREATE PERFETTO TABLE b AS
        SELECT 10 AS ts, 10 AS dur, 2 AS cpu
        UNION ALL SELECT 10, 10, 3;

        INTERVAL INTERSECTION OF (a AS x, b AS y) PER cpu
        |> SELECT ts, dur, cpu;
        """,
        out=Csv("""
        "ts","dur","cpu"
        10,10,2
        """))

  # Rows holding no value in a PER column agree on it, as they would under
  # GROUP BY, which is what the macro this replaces does.
  def test_interval_intersection_per_an_absent_key(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS
        SELECT 0 AS ts, 30 AS dur, 1 AS cpu, 'one' AS name
        UNION ALL SELECT 0, 30, NULL, 'two';

        CREATE PERFETTO TABLE b AS
        SELECT 10 AS ts, 10 AS dur, 1 AS cpu
        UNION ALL SELECT 10, 10, NULL;

        CREATE PERFETTO TABLE met AS
        INTERVAL INTERSECTION OF (a AS x, b AS y) PER cpu
        |> SELECT ts, dur, cpu, x.name;

        SELECT ts, dur, cpu, name FROM met ORDER BY name;
        """,
        out=Csv("""
        "ts","dur","cpu","name"
        10,10,1,"one"
        10,10,"[NULL]","two"
        """))

  # Two intervals meet over the instants they share, so touching end to start
  # is no meeting at all.
  def test_interval_intersection_touching_is_no_meeting(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 10 AS dur;
        CREATE PERFETTO TABLE b AS SELECT 10 AS ts, 10 AS dur;

        INTERVAL INTERSECTION OF (a AS x, b AS y) |> SELECT ts, dur;
        """,
        out=Csv("""
        "ts","dur"
        """))

  # A row of no width is a point: it meets an interval holding the instant it
  # sits at, and the interval's end is not such an instant.
  def test_interval_intersection_point_positions(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        #        5    10   15   19   20   25
        # o:          [--------------)
        # p:     .    .    .    .    .    .
        # res:        x    x    x
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE o AS SELECT 10 AS ts, 10 AS dur;

        CREATE PERFETTO TABLE p AS
        SELECT 5 AS ts, 0 AS dur
        UNION ALL SELECT 10, 0
        UNION ALL SELECT 15, 0
        UNION ALL SELECT 19, 0
        UNION ALL SELECT 20, 0
        UNION ALL SELECT 25, 0;

        CREATE PERFETTO TABLE met AS
        INTERVAL INTERSECTION OF (p AS x, o AS y) |> SELECT ts, dur;

        SELECT ts, dur FROM met ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur"
        10,0
        15,0
        19,0
        """))

  # Narrowing an interval down to a point keeps the point, and a point which
  # no longer meets anything takes the region with it.
  def test_interval_intersection_through_three_tables(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 100 AS dur;
        CREATE PERFETTO TABLE b AS SELECT 20 AS ts, 10 AS dur;
        CREATE PERFETTO TABLE c AS
        SELECT 25 AS ts, 0 AS dur UNION ALL SELECT 60, 0;

        INTERVAL INTERSECTION OF (a AS x, b AS y, c AS z)
        |> SELECT ts, dur;
        """,
        out=Csv("""
        "ts","dur"
        25,0
        """))

  # A duration which never ends is refused rather than read as a span.
  def test_interval_intersection_refuses_a_negative_duration(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 10 AS dur;
        CREATE PERFETTO TABLE b AS SELECT 5 AS ts, -1 AS dur;

        INTERVAL INTERSECTION OF (a AS x, b AS y) |> SELECT ts, dur;
        """,
        out=ExpectedError('ts or dur is below zero'))

  def test_interval_intersection_needs_the_pragma(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 10 AS dur;
        CREATE PERFETTO TABLE b AS SELECT 5 AS ts, 10 AS dur;

        INTERVAL INTERSECTION OF (a AS x, b AS y) |> SELECT ts, dur;
        """,
        out=ExpectedError('Pipelines are not enabled'))

  # Must match the macro, which this is meant to replace.
  def test_interval_intersection_matches_the_macro(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE running AS
        SELECT id, ts, dur, cpu FROM sched WHERE dur > 0;

        CREATE PERFETTO TABLE freq AS
        SELECT * FROM (
          SELECT
            c.id,
            c.ts,
            lead(c.ts) OVER (PARTITION BY t.cpu ORDER BY c.ts) - c.ts AS dur,
            t.cpu,
            cast_int!(c.value) AS freq
          FROM counter c
          JOIN cpu_counter_track t ON c.track_id = t.id
          WHERE t.name = 'cpufreq'
        ) WHERE dur > 0;

        CREATE PERFETTO TABLE piped AS
        INTERVAL INTERSECTION OF (running AS r, freq AS f) PER cpu
        |> SELECT ts, dur, cpu, r.id AS sched_id, f.freq;

        CREATE PERFETTO TABLE macroed AS
        SELECT ii.ts, ii.dur, ii.cpu, r.id AS sched_id, f.freq
        FROM _interval_intersect!((running, freq), (cpu)) ii
        JOIN running r ON r.id = ii.id_0
        JOIN freq f ON f.id = ii.id_1;

        SELECT
          (SELECT count(*) FROM piped) AS rows,
          (
            SELECT count(*) FROM (
              SELECT * FROM piped EXCEPT SELECT * FROM macroed
              UNION ALL
              SELECT * FROM macroed EXCEPT SELECT * FROM piped
            )
          ) AS mismatches;
        """,
        out=Csv("""
        "rows","mismatches"
        45728,0
        """))
