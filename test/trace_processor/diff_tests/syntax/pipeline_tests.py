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
