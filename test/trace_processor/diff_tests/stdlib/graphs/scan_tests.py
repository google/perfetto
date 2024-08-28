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


class GraphScanTests(TestSuite):

  def test_scan_empty(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.scan;

          WITH foo AS (
            SELECT 0 as source_node_id, 0 AS dest_node_id
            WHERE FALSE
          )
          SELECT * FROM _graph_scan!(
            foo,
            (SELECT 0 AS id, 0 as depth WHERE FALSE),
            (depth),
            (
              select id, depth + 1 as depth
              from $table
            )
          )
        """,
        out=Csv("""
        "id","depth"
        """))

  def test_scan_single_row(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.scan;

          WITH foo AS (
            SELECT 0 as source_node_id, 0 AS dest_node_id
            WHERE FALSE
          )
          SELECT * FROM _graph_scan!(
            foo,
            (SELECT 0 AS id, 0 as depth),
            (depth),
            (
              select id, depth + 1 as depth
              from $table
            )
          )
        """,
        out=Csv("""
        "id","depth"
        0,0
        """))

  def test_scan_root_depth(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.scan;

          WITH
            edges(source_node_id, dest_node_id) AS (
              VALUES(0, 1), (0, 2), (1, 2), (2, 3)
            ),
            init(id, root_id, depth) AS (
              VALUES(0, 0, 0), (1, 1, 0)
            )
          SELECT * FROM _graph_scan!(
            edges,
            init,
            (root_id, depth),
            (
              SELECT id, root_id, depth + 1 as depth
              FROM $table
            )
          )
          ORDER BY id, root_id
        """,
        out=Csv("""
        "id","root_id","depth"
        0,0,0
        1,0,1
        1,1,0
        2,0,1
        2,0,2
        2,1,1
        3,0,2
        3,0,3
        3,1,2
        """))

  def test_aggregating_scan_empty(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.scan;

          WITH foo AS (
            SELECT 0 as source_node_id, 0 AS dest_node_id
            WHERE FALSE
          )
          SELECT * FROM _graph_aggregating_scan!(
            foo,
            (SELECT 0 AS id, 0 as depth WHERE FALSE),
            (depth),
            (
              select id, depth + 1 as depth
              from $table
            )
          )
        """,
        out=Csv("""
        "id","depth"
        """))

  def test_aggregating_scan_single_row(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.scan;

          WITH foo AS (
            SELECT 0 as source_node_id, 0 AS dest_node_id
            WHERE FALSE
          )
          SELECT * FROM _graph_aggregating_scan!(
            foo,
            (SELECT 0 AS id, 0 as depth),
            (depth),
            (
              select id, depth + 1 as depth
              from $table
            )
          )
        """,
        out=Csv("""
        "id","depth"
        0,0
        """))

  def test_aggregating_scan_max_recursive(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.scan;

          WITH
            edges(source_node_id, dest_node_id) AS (
              VALUES(0, 1), (0, 2), (1, 2), (2, 3), (4, 5)
            ),
            init(id, max_depth) AS (
              VALUES(0, 0), (4, 0)
            )
          SELECT * FROM _graph_aggregating_scan!(
            edges,
            init,
            (max_depth),
            (
              SELECT id, MAX(max_depth) + 1 as max_depth
              FROM $table
              GROUP BY id
            )
          )
          ORDER BY id
        """,
        out=Csv("""
        "id","max_depth"
        0,0
        1,1
        2,2
        3,3
        4,0
        5,1
        """))

  def test_aggregating_scan_min_recursive(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.scan;

          WITH
            edges(source_node_id, dest_node_id) AS (
              VALUES(0, 1), (0, 2), (1, 2), (2, 3), (4, 5)
            ),
            init(id, min_depth) AS (
              VALUES(0, 0), (4, 0)
            )
          SELECT * FROM _graph_aggregating_scan!(
            edges,
            init,
            (min_depth),
            (
              SELECT id, MIN(min_depth) + 1 as min_depth
              FROM $table
              GROUP BY id
            )
          )
          ORDER BY id
        """,
        out=Csv("""
        "id","min_depth"
        0,0
        1,1
        2,1
        3,2
        4,0
        5,1
        """))
