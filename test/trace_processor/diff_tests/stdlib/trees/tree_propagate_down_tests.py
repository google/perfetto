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
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class TreePropagateDown(TestSuite):
  """Tests for tree propagate_down operation."""

  def test_sum(self):
    """Sum propagation: child += parent."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.propagate_down;

          CREATE PERFETTO TABLE input_tree AS
          SELECT 1 AS id, NULL AS parent_id, 10 AS value
          UNION ALL SELECT 2, 1, 5
          UNION ALL SELECT 3, 1, 3
          UNION ALL SELECT 4, 2, 2;

          SELECT _tree_id, id, value, cumulative
          FROM _tree_to_table!(
            _tree_propagate_down(
              _tree_from_table!((SELECT * FROM input_tree), (value)),
              'value',
              'sum',
              'cumulative'
            ),
            (value, cumulative)
          )
          ORDER BY id;
        """,
        out=Csv("""
        "_tree_id","id","value","cumulative"
        0,1,10,10
        1,2,5,15
        2,3,3,13
        3,4,2,17
        """))

  def test_min(self):
    """Min propagation: child = min(parent, child)."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.propagate_down;

          CREATE PERFETTO TABLE input_tree AS
          SELECT 1 AS id, NULL AS parent_id, 5 AS value
          UNION ALL SELECT 2, 1, 3
          UNION ALL SELECT 3, 1, 8
          UNION ALL SELECT 4, 2, 1;

          SELECT _tree_id, id, value, result
          FROM _tree_to_table!(
            _tree_propagate_down(
              _tree_from_table!((SELECT * FROM input_tree), (value)),
              'value',
              'min',
              'result'
            ),
            (value, result)
          )
          ORDER BY id;
        """,
        out=Csv("""
        "_tree_id","id","value","result"
        0,1,5,5
        1,2,3,3
        2,3,8,5
        3,4,1,1
        """))

  def test_max(self):
    """Max propagation: child = max(parent, child)."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.propagate_down;

          CREATE PERFETTO TABLE input_tree AS
          SELECT 1 AS id, NULL AS parent_id, 5 AS value
          UNION ALL SELECT 2, 1, 3
          UNION ALL SELECT 3, 1, 8
          UNION ALL SELECT 4, 2, 10;

          SELECT _tree_id, id, value, result
          FROM _tree_to_table!(
            _tree_propagate_down(
              _tree_from_table!((SELECT * FROM input_tree), (value)),
              'value',
              'max',
              'result'
            ),
            (value, result)
          )
          ORDER BY id;
        """,
        out=Csv("""
        "_tree_id","id","value","result"
        0,1,5,5
        1,2,3,5
        2,3,8,8
        3,4,10,10
        """))

  def test_first(self):
    """First propagation: child = parent (parent overwrites)."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.propagate_down;

          CREATE PERFETTO TABLE input_tree AS
          SELECT 1 AS id, NULL AS parent_id, 100 AS value
          UNION ALL SELECT 2, 1, 200
          UNION ALL SELECT 3, 2, 300
          UNION ALL SELECT 4, 1, 400;

          SELECT _tree_id, id, value, result
          FROM _tree_to_table!(
            _tree_propagate_down(
              _tree_from_table!((SELECT * FROM input_tree), (value)),
              'value',
              'first',
              'result'
            ),
            (value, result)
          )
          ORDER BY id;
        """,
        out=Csv("""
        "_tree_id","id","value","result"
        0,1,100,100
        1,2,200,100
        2,3,300,100
        3,4,400,100
        """))

  def test_last(self):
    """Last propagation: child keeps its own value."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.propagate_down;

          CREATE PERFETTO TABLE input_tree AS
          SELECT 1 AS id, NULL AS parent_id, 100 AS value
          UNION ALL SELECT 2, 1, 200
          UNION ALL SELECT 3, 2, 300
          UNION ALL SELECT 4, 1, 400;

          SELECT _tree_id, id, value, result
          FROM _tree_to_table!(
            _tree_propagate_down(
              _tree_from_table!((SELECT * FROM input_tree), (value)),
              'value',
              'last',
              'result'
            ),
            (value, result)
          )
          ORDER BY id;
        """,
        out=Csv("""
        "_tree_id","id","value","result"
        0,1,100,100
        1,2,200,200
        2,3,300,300
        3,4,400,400
        """))
