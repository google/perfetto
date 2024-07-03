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

class CriticalPathTests(TestSuite):

  def test_critical_path_empty(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.critical_path;

          WITH edge AS (
            SELECT 0 as source_node_id, 0 AS dest_node_id
            WHERE FALSE
          ), root AS (
            SELECT 0 as root_node_id, 0 AS capacity
            WHERE FALSE
          )
          SELECT * FROM _critical_path!(
            (SELECT *, source_node_id - dest_node_id AS edge_weight FROM edge),
            root
          );
        """,
        out=Csv("""
        "root_id","parent_id","id"
        """))

  def test_critical_path(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.critical_path;

          WITH edge(source_node_id, dest_node_id) AS (
            values(8, 7), (7, 6), (6, 5), (6, 4), (4, 1), (5, 3), (3, 0)
          ), root(root_node_id, capacity) AS (
            values(8, 6)
          )
          SELECT * FROM _critical_path!(
            (SELECT *, source_node_id - dest_node_id AS edge_weight FROM edge),
            root
          );
        """,
        out=Csv("""
        "root_id","parent_id","id"
        8,"[NULL]",8
        8,3,0
        8,5,3
        8,6,5
        8,7,6
        8,8,7
        """))

  def test_critical_path_intervals(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.critical_path;

          WITH edge(source_node_id, dest_node_id) AS (
            values(8, 7), (7, 6), (6, 5), (6, 4), (4, 1), (5, 3), (3, 0)
          ), root(root_node_id, capacity) AS (
            values(8, 6)
          ), interval(id, ts, dur, idle_dur) AS (
            values(8, 8, 1, 6),
                  (7, 7, 1, 1),
                  (6, 6, 1, 1),
                  (5, 5, 1, 1),
                  (4, 4, 1, 1),
                  (3, 3, 1, 1),
                  (2, 2, 1, 1),
                  (1, 1, 1, 1)
          )
          SELECT * FROM _critical_path_intervals!(
            (SELECT *, source_node_id - dest_node_id AS edge_weight FROM edge),
            root,
            interval
          );
        """,
        out=Csv("""
        "root_id","id","ts","dur"
        8,3,3,2
        8,5,5,1
        8,6,6,1
        8,7,7,1
        8,8,8,1
        """))
