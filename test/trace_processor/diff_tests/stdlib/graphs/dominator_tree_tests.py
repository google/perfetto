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

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class DominatorTree(TestSuite):

  def test_empty_table(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.dominator_tree;

          WITH foo AS (
            SELECT 0 as source_node_id, 0 AS dest_node_id
            WHERE FALSE
          )
          SELECT * FROM graph_dominator_tree!(foo, NULL)
        """,
        out=Csv("""
        "node_id","dominator_node_id"
        """))

  def test_one_node(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.dominator_tree;

          WITH foo AS (
            SELECT 5 AS source_node_id, 10 AS dest_node_id
            UNION ALL
            SELECT 10, 10
          )
          SELECT * FROM graph_dominator_tree!(foo, 5);
        """,
        out=Csv("""
        "node_id","dominator_node_id"
        5,"[NULL]"
        10,5
        """))

  def test_two_nodes(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.dominator_tree;

          CREATE PERFETTO TABLE foo AS
          SELECT NULL AS source_node_id, NULL AS dest_node_id WHERE FALSE
          UNION ALL
          VALUES (10, 11)
          UNION ALL
          VALUES (0, 10);

          SELECT * FROM graph_dominator_tree!(foo, 0);
        """,
        out=Csv("""
        "node_id","dominator_node_id"
        0,"[NULL]"
        10,0
        11,10
        """))

  def test_lengauer_tarjan_example(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.dominator_tree;

          CREATE PERFETTO TABLE foo AS
          SELECT NULL AS source, NULL AS dest WHERE FALSE
          UNION ALL
          VALUES ('R', 'A'), ('R', 'B'), ('R', 'C'), ('A', 'D')
          UNION ALL
          VALUES ('B', 'A'), ('B', 'D'), ('B', 'E'), ('C', 'F')
          UNION ALL
          VALUES ('C', 'G'), ('D', 'L'), ('E', 'H'), ('F', 'I')
          UNION ALL
          VALUES ('G', 'I'), ('G', 'J'), ('H', 'E'), ('H', 'K')
          UNION ALL
          VALUES ('I', 'K'), ('J', 'I'), ('K', 'I'), ('K', 'R')
          UNION ALL
          VALUES ('L', 'H');

          WITH bar AS (
            SELECT
              unicode(source) AS source_node_id,
              unicode(dest) AS dest_node_id
            FROM foo
          )
          SELECT
            char(node_id) AS node_id,
            IIF(
              dominator_node_id IS NULL,
              NULL,
              char(dominator_node_id)
            ) AS dominator_node_id
          FROM graph_dominator_tree!(bar, unicode('R'))
          ORDER BY node_id;
        """,
        out=Csv("""
        "node_id","dominator_node_id"
        "A","R"
        "B","R"
        "C","R"
        "D","R"
        "E","R"
        "F","C"
        "G","C"
        "H","R"
        "I","R"
        "J","G"
        "K","R"
        "L","D"
        "R","[NULL]"
        """))

  def test_small_complete_graph(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.dominator_tree;

          CREATE PERFETTO TABLE foo AS
          SELECT NULL AS source_node_id, NULL AS dest_node_id WHERE FALSE
          UNION ALL
          VALUES (1, 10), (10, 10), (10, 11), (10, 12)
          UNION ALL
          VALUES (11, 10), (11, 11), (11, 12), (12, 10)
          UNION ALL
          VALUES (12, 11), (12, 12)
          UNION ALL
          VALUES (1, 10);

          SELECT * FROM graph_dominator_tree!(foo, 1) ORDER BY node_id;
        """,
        out=Csv("""
        "node_id","dominator_node_id"
        1,"[NULL]"
        10,1
        11,10
        12,10
        """))

  def test_forest(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.dominator_tree;

          CREATE PERFETTO TABLE foo AS
          SELECT NULL AS source_node_id, NULL AS dest_node_id WHERE FALSE
          UNION ALL
          VALUES (1, 2), (1, 3), (2, 4), (2, 5), (3, 6), (3, 6), (3, 6)
          UNION ALL
          VALUES (3, 7), (11, 12), (11, 13), (12, 14), (12, 15), (13, 16)
          UNION ALL
          VALUES (21, 22), (22, 24)
          UNION ALL
          VALUES (0, 1), (0, 11), (0, 21);

          SELECT *
          FROM graph_dominator_tree!(foo, 0)
          ORDER BY node_id;
        """,
        out=Csv("""
        "node_id","dominator_node_id"
        0,"[NULL]"
        1,0
        2,1
        3,1
        4,2
        5,2
        6,3
        7,3
        11,0
        12,11
        13,11
        14,12
        15,12
        16,13
        21,0
        22,21
        24,22
        """))
