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


class GraphSearchTests(TestSuite):

  def test_dfs_empty_table(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.search;

          WITH foo AS (
            SELECT 0 as source_node_id, 0 AS dest_node_id
            WHERE FALSE
          )
          SELECT * FROM graph_reachable_dfs!(
            foo,
            (SELECT 0 AS node_id WHERE FALSE)
          )
        """,
        out=Csv("""
        "node_id","parent_node_id"
        """))

  def test_dfs_one_node(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.search;

          WITH foo AS (
            SELECT 5 AS source_node_id, 10 AS dest_node_id
            UNION ALL
            SELECT 10, 10
          )
          SELECT * FROM graph_reachable_dfs!(foo, (SELECT 5 AS node_id));
        """,
        out=Csv("""
        "node_id","parent_node_id"
        5,"[NULL]"
        10,5
        """))

  def test_dfs_two_nodes(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.search;

          CREATE PERFETTO TABLE foo AS
          SELECT NULL AS source_node_id, NULL AS dest_node_id WHERE FALSE
          UNION ALL
          VALUES (10, 11)
          UNION ALL
          VALUES (0, 10);

          SELECT * FROM graph_reachable_dfs!(foo, (SELECT 0 AS node_id));
        """,
        out=Csv("""
        "node_id","parent_node_id"
        0,"[NULL]"
        10,0
        11,10
        """))

  def test_dfs_lengauer_tarjan_example(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.search;

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
              parent_node_id IS NULL,
              NULL,
              char(parent_node_id)
            ) AS parent_node_id
          FROM graph_reachable_dfs!(bar, (SELECT unicode('R') AS node_id))
          ORDER BY node_id;
        """,
        out=Csv("""
          "node_id","parent_node_id"
          "A","R"
          "B","R"
          "C","R"
          "D","A"
          "E","H"
          "F","C"
          "G","C"
          "H","L"
          "I","K"
          "J","G"
          "K","H"
          "L","D"
          "R","[NULL]"
        """))

  def test_bfs_empty_table(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.search;

          WITH foo AS (
            SELECT 0 as source_node_id, 0 AS dest_node_id
            WHERE FALSE
          )
          SELECT * FROM graph_reachable_bfs!(
            foo, (SELECT 0 AS node_id WHERE FALSE)
          )
        """,
        out=Csv("""
        "node_id","parent_node_id"
        """))

  def test_bfs_one_node(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.search;

          WITH foo AS (
            SELECT 5 AS source_node_id, 10 AS dest_node_id
            UNION ALL
            SELECT 10, 10
          )
          SELECT * FROM graph_reachable_bfs!(foo, (SELECT 5 AS node_id));
        """,
        out=Csv("""
        "node_id","parent_node_id"
        5,"[NULL]"
        10,5
        """))

  def test_bfs_two_nodes(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.search;

          CREATE PERFETTO TABLE foo AS
          SELECT NULL AS source_node_id, NULL AS dest_node_id WHERE FALSE
          UNION ALL
          VALUES (10, 11)
          UNION ALL
          VALUES (0, 10);

          SELECT * FROM graph_reachable_bfs!(foo, (SELECT 0 AS node_id));
        """,
        out=Csv("""
        "node_id","parent_node_id"
        0,"[NULL]"
        10,0
        11,10
        """))

  def test_bfs_lengauer_tarjan_example(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.search;

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
              parent_node_id IS NULL,
              NULL,
              char(parent_node_id)
            ) AS parent_node_id
          FROM graph_reachable_bfs!(bar, (SELECT unicode('R') AS node_id))
          ORDER BY node_id;
        """,
        out=Csv("""
          "node_id","parent_node_id"
          "A","R"
          "B","R"
          "C","R"
          "D","A"
          "E","B"
          "F","C"
          "G","C"
          "H","E"
          "I","F"
          "J","G"
          "K","H"
          "L","D"
          "R","[NULL]"
        """))

  def test_next_sibling(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.search;

          CREATE PERFETTO TABLE foo AS
          SELECT 1 AS node_id, 0 AS node_parent_id, 1 AS sort_key
          UNION ALL
          SELECT 2 AS node_id, 1 AS node_parent_id, 2 AS sort_key
          UNION ALL
          SELECT 3 AS node_id, 1 AS node_parent_id, 1 AS sort_key;

          SELECT * FROM graph_next_sibling!(foo);
        """,
        out=Csv("""
        "node_id","next_node_id"
        1,"[NULL]"
        3,2
        2,"[NULL]"
        """))

  def test_weight_bounded_dfs_floor(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.search;

          CREATE PERFETTO TABLE foo AS
          SELECT 0 AS source_node_id, 0 AS dest_node_id, 0 AS edge_weight
          UNION ALL
          VALUES (1, 2, 1)
          UNION ALL
          VALUES (1, 3, 1)
          UNION ALL
          VALUES (3, 4, 1)
          UNION ALL
          VALUES (3, 5, 0)
          UNION ALL
          VALUES (5, 6, 0);

          CREATE PERFETTO TABLE roots AS
          SELECT 0 AS root_node_id, 0 AS root_target_weight
          UNION ALL
          VALUES (1, 2)
          UNION ALL
          VALUES (3, 1)
          UNION ALL
          VALUES (2, 0);

          SELECT * FROM graph_reachable_weight_bounded_dfs!(foo, roots, 1);
        """,
        out=Csv("""
        "root_node_id","node_id","parent_node_id"
        0,0,"[NULL]"
        1,1,"[NULL]"
        1,2,1
        1,3,1
        1,4,3
        3,3,"[NULL]"
        3,4,3
        3,5,3
        3,6,5
        2,2,"[NULL]"
        """))

  def test_weight_bounded_dfs_ceiling(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.search;

          CREATE PERFETTO TABLE foo AS
          SELECT 0 AS source_node_id, 0 AS dest_node_id, 0 AS edge_weight
          UNION ALL
          VALUES (1, 2, 1)
          UNION ALL
          VALUES (1, 3, 1)
          UNION ALL
          VALUES (3, 4, 1)
          UNION ALL
          VALUES (3, 5, 0)
          UNION ALL
          VALUES (5, 6, 0);

          CREATE PERFETTO TABLE roots AS
          SELECT 0 AS root_node_id, 0 AS root_target_weight
          UNION ALL
          VALUES (1, 2)
          UNION ALL
          VALUES (3, 1)
          UNION ALL
          VALUES (2, 0);

          SELECT * FROM graph_reachable_weight_bounded_dfs!(foo, roots, 0);
        """,
        out=Csv("""
        "root_node_id","node_id","parent_node_id"
        0,0,"[NULL]"
        1,1,"[NULL]"
        1,2,1
        1,3,1
        3,3,"[NULL]"
        3,4,3
        3,5,3
        3,6,5
        2,2,"[NULL]"
        """))
