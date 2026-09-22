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
        FROM (SELECT 1 AS id, NULL AS parent_id)
        |> TREE ACCUMULATE UP MAX(id) AS biggest;
        """,
        out=ExpectedError('aggregate MAX is not supported yet'))

  # `|>` is one token, so `|` keeps its own meaning either side of a pipe.
  def test_pipe_is_its_own_token(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        CREATE PERFETTO TABLE no_spaces AS
        FROM (SELECT 4 AS v) |>SELECT v;

        SELECT
          (SELECT 1 | 2 > 0) AS plain_sql,
          (SELECT v FROM no_spaces) AS no_spaces,
          (SELECT '|>') AS in_a_string;
        """,
        out=Csv("""
        "plain_sql","no_spaces","in_a_string"
        1,4,"|>"
        """))

  # A stage ending in `|` reaches the compiler rather than failing to parse,
  # which is what having a PIPE token buys.
  def test_a_stage_may_end_in_a_bitwise_or(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        FROM (SELECT 1 AS a, 6 AS b)
        |> SELECT a | b AS v;
        """,
        out=ExpectedError('a computed column is not supported yet'))

  def test_pipe_outside_a_pipeline_is_a_syntax_error(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT 1 |> 2;
        """,
        out=ExpectedError('syntax error'))

  def test_select_picks_renames_and_reorders(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        FROM (SELECT 1 AS a, 2 AS b, 3 AS c) AS t
        |> SELECT c, t.a AS first, b;
        """,
        out=Csv("""
        "c","first","b"
        3,1,2
        """))

  # An aggregate has its own stage, so this is not a gap to be filled later.
  def test_select_refuses_an_aggregate(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        FROM (SELECT 1 AS dur)
        |> SELECT SUM(dur) AS total;
        """,
        out=ExpectedError('an aggregate belongs in AGGREGATE, not SELECT'))

  def test_select_refuses_a_starred_aggregate(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        FROM (SELECT 1 AS dur)
        |> SELECT count(*) AS n;
        """,
        out=ExpectedError('an aggregate belongs in AGGREGATE, not SELECT'))

  def test_select_does_not_compute_columns_yet(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        FROM (SELECT 1 AS dur)
        |> SELECT dur + 1 AS longer;
        """,
        out=ExpectedError('a computed column is not supported yet'))

  def test_interval_join_relationships(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        CREATE PERFETTO TABLE frames AS
        SELECT 0 AS id, 0 AS ts, 10 AS dur, 1 AS utid
        UNION ALL SELECT 1, 10, 10, 1
        UNION ALL SELECT 2, 12, 2, 2
        UNION ALL SELECT 3, 50, 5, 1;

        CREATE PERFETTO TABLE cujs AS
        SELECT 'scroll' AS name, 5 AS ts, 10 AS dur, 1 AS utid
        UNION ALL SELECT 'open', 10, 30, 1
        UNION ALL SELECT 'other', 0, 100, 2;

        CREATE PERFETTO TABLE joined AS
        FROM frames AS f
        |> INTERVAL JOIN cujs AS c OVERLAPPING BOUNDS PER utid
        |> SELECT id, c.name AS cuj, c.ts AS cuj_ts;

        CREATE PERFETTO TABLE covered AS
        FROM frames AS f
        |> LEFT INTERVAL JOIN (SELECT * FROM cujs WHERE name != 'other') AS c
           COVERING BOUNDS PER utid
        |> SELECT id, c.name AS cuj;

        SELECT 'overlapping' AS kind, id, cuj, cuj_ts FROM joined
        UNION ALL
        SELECT 'covering', id, cuj, NULL FROM covered
        ORDER BY kind, id, cuj;
        """,
        out=Csv("""
        "kind","id","cuj","cuj_ts"
        "covering",0,"[NULL]","[NULL]"
        "covering",1,"open","[NULL]"
        "covering",2,"[NULL]","[NULL]"
        "covering",3,"[NULL]","[NULL]"
        "overlapping",0,"scroll",5
        "overlapping",1,"open",10
        "overlapping",1,"scroll",5
        "overlapping",2,"other",0
        """))

  # Must match the range join in SQL which this replaces.
  def test_interval_join_matches_a_range_join(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        CREATE PERFETTO TABLE windows AS
        SELECT id AS window_id, ts, dur, track_id
        FROM slice
        WHERE depth = 0 AND dur > 0;

        CREATE PERFETTO TABLE piped AS
        FROM slice
        |> INTERVAL JOIN windows AS w COVERING BEGIN PER track_id
        |> SELECT id, w.window_id AS window_id;

        CREATE PERFETTO TABLE joined AS
        SELECT s.id, w.window_id
        FROM slice AS s
        JOIN windows AS w
          ON s.track_id = w.track_id AND s.ts >= w.ts AND s.ts < w.ts + w.dur;

        SELECT
          (SELECT count(*) FROM piped) AS rows,
          (SELECT count(*) FROM joined) AS expected_rows,
          (
            SELECT count(*)
            FROM (SELECT * FROM piped EXCEPT SELECT * FROM joined)
          ) AS mismatches;
        """,
        out=Csv("""
        "rows","expected_rows","mismatches"
        70964,70964,0
        """))

  def test_interval_join_operand_columns_need_their_alias(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        FROM (SELECT 0 AS ts, 1 AS dur) AS a
        |> INTERVAL JOIN (SELECT 0 AS ts, 1 AS dur, 'x' AS name) AS b
           OVERLAPPING BOUNDS
        |> SELECT ts, name;
        """,
        out=ExpectedError("no such column: 'name'"))

  # An operand needs an alias to name its columns by. Leaving it out reads as
  # one, since a relationship begins with a word a name could be.
  def test_interval_join_needs_an_alias_for_its_operand(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        FROM (SELECT 0 AS ts, 1 AS dur)
        |> INTERVAL JOIN (SELECT 0 AS ts, 1 AS dur) OVERLAPPING BOUNDS;
        """,
        out=ExpectedError("syntax error near 'BOUNDS'"))
