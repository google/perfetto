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


class TreePropagate(TestSuite):
  """Tests for tree propagate_down operations."""

  def test_propagate_down_depth_sum(self):
    """SUM(ones) AS depth computes depth: root=1, children=2, etc."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.propagate;

          CREATE PERFETTO TABLE calls AS
          SELECT 1 AS id, NULL AS parent_id, 'main' AS fn, 1 AS ones
          UNION ALL SELECT 2, 1, 'parse', 1
          UNION ALL SELECT 3, 2, 'lex', 1
          UNION ALL SELECT 4, 1, 'emit', 1
          UNION ALL SELECT 5, 4, 'write', 1;

          SELECT fn, depth
          FROM _tree_to_table!(
            _tree_propagate_down(
              _tree_from_table!((SELECT * FROM calls), (fn, ones)),
              'SUM(ones) AS depth'
            ),
            (fn, depth)
          )
          ORDER BY fn;
        """,
        out=Csv("""
        "fn","depth"
        "emit",2
        "lex",3
        "main",1
        "parse",2
        "write",3
        """))

  def test_propagate_down_multiple_columns(self):
    """Multiple specs in one call: SUM + MAX."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.propagate;

          CREATE PERFETTO TABLE calls AS
          SELECT 1 AS id, NULL AS parent_id, 'main' AS fn, 1 AS ones, 10 AS prio
          UNION ALL SELECT 2, 1, 'parse', 1, 5
          UNION ALL SELECT 3, 2, 'lex', 1, 8
          UNION ALL SELECT 4, 1, 'emit', 1, 3;

          SELECT fn, depth, max_prio
          FROM _tree_to_table!(
            _tree_propagate_down(
              _tree_from_table!((SELECT * FROM calls), (fn, ones, prio)),
              'SUM(ones) AS depth',
              'MAX(prio) AS max_prio'
            ),
            (fn, depth, max_prio)
          )
          ORDER BY fn;
        """,
        out=Csv("""
        "fn","depth","max_prio"
        "emit",2,10
        "lex",3,10
        "main",1,10
        "parse",2,10
        """))

  def test_propagate_down_min(self):
    """MIN propagation: each node gets min of its value and parent's propagated value."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.propagate;

          CREATE PERFETTO TABLE calls AS
          SELECT 1 AS id, NULL AS parent_id, 'root' AS fn, 5 AS val
          UNION ALL SELECT 2, 1, 'a', 10
          UNION ALL SELECT 3, 2, 'b', 3
          UNION ALL SELECT 4, 1, 'c', 2;

          SELECT fn, min_val
          FROM _tree_to_table!(
            _tree_propagate_down(
              _tree_from_table!((SELECT * FROM calls), (fn, val)),
              'MIN(val) AS min_val'
            ),
            (fn, min_val)
          )
          ORDER BY fn;
        """,
        out=Csv("""
        "fn","min_val"
        "a",5
        "b",3
        "c",2
        "root",5
        """))

  def test_propagate_down_first(self):
    """FIRST propagation: each node gets the root's value."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.propagate;

          CREATE PERFETTO TABLE calls AS
          SELECT 1 AS id, NULL AS parent_id, 'root' AS fn, 100 AS val
          UNION ALL SELECT 2, 1, 'a', 50
          UNION ALL SELECT 3, 2, 'b', 25;

          SELECT fn, first_val
          FROM _tree_to_table!(
            _tree_propagate_down(
              _tree_from_table!((SELECT * FROM calls), (fn, val)),
              'FIRST(val) AS first_val'
            ),
            (fn, first_val)
          )
          ORDER BY fn;
        """,
        out=Csv("""
        "fn","first_val"
        "a",100
        "b",100
        "root",100
        """))

  def test_propagate_down_last(self):
    """LAST propagation: each node keeps its own value (no-op)."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.propagate;

          CREATE PERFETTO TABLE calls AS
          SELECT 1 AS id, NULL AS parent_id, 'root' AS fn, 100 AS val
          UNION ALL SELECT 2, 1, 'a', 50
          UNION ALL SELECT 3, 2, 'b', 25;

          SELECT fn, last_val
          FROM _tree_to_table!(
            _tree_propagate_down(
              _tree_from_table!((SELECT * FROM calls), (fn, val)),
              'LAST(val) AS last_val'
            ),
            (fn, last_val)
          )
          ORDER BY fn;
        """,
        out=Csv("""
        "fn","last_val"
        "a",50
        "b",25
        "root",100
        """))

  def test_propagate_down_after_filter(self):
    """Propagation after filtering: filter first, then propagate depth."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.filter;
          INCLUDE PERFETTO MODULE std.trees.propagate;

          CREATE PERFETTO TABLE calls AS
          SELECT 1 AS id, NULL AS parent_id, 'main' AS fn, 100 AS dur, 1 AS ones
          UNION ALL SELECT 2, 1, 'parse', 60, 1
          UNION ALL SELECT 3, 2, 'lex', 30, 1
          UNION ALL SELECT 4, 1, 'alloc', 5, 1
          UNION ALL SELECT 5, 1, 'emit', 35, 1;

          SELECT fn, depth
          FROM _tree_to_table!(
            _tree_propagate_down(
              _tree_filter(
                _tree_from_table!((SELECT * FROM calls), (fn, dur, ones)),
                _tree_where(_tree_constraint('dur', '>=', 10))
              ),
              'SUM(ones) AS depth'
            ),
            (fn, depth)
          )
          ORDER BY fn;
        """,
        out=Csv("""
        "fn","depth"
        "emit",2
        "lex",3
        "main",1
        "parse",2
        """))

  def test_propagate_down_multiple_roots(self):
    """Propagation with a forest (multiple roots)."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.propagate;

          CREATE PERFETTO TABLE calls AS
          SELECT 1 AS id, NULL AS parent_id, 'tree1' AS fn, 1 AS ones
          UNION ALL SELECT 2, 1, 'a', 1
          UNION ALL SELECT 3, NULL, 'tree2', 1
          UNION ALL SELECT 4, 3, 'b', 1;

          SELECT fn, depth
          FROM _tree_to_table!(
            _tree_propagate_down(
              _tree_from_table!((SELECT * FROM calls), (fn, ones)),
              'SUM(ones) AS depth'
            ),
            (fn, depth)
          )
          ORDER BY fn;
        """,
        out=Csv("""
        "fn","depth"
        "a",2
        "b",2
        "tree1",1
        "tree2",1
        """))

  def test_propagate_down_single_node(self):
    """Edge case: single-node tree."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.propagate;

          CREATE PERFETTO TABLE calls AS
          SELECT 1 AS id, NULL AS parent_id, 'root' AS fn, 42 AS val;

          SELECT fn, s
          FROM _tree_to_table!(
            _tree_propagate_down(
              _tree_from_table!((SELECT * FROM calls), (fn, val)),
              'SUM(val) AS s'
            ),
            (fn, s)
          )
          ORDER BY fn;
        """,
        out=Csv("""
        "fn","s"
        "root",42
        """))

  def test_propagate_down_filter_then_propagate_then_filter(self):
    """filter → propagate → filter(propagated_col) chain."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.filter;
          INCLUDE PERFETTO MODULE std.trees.propagate;

          CREATE PERFETTO TABLE calls AS
          SELECT 1 AS id, NULL AS parent_id, 'main' AS fn, 100 AS dur, 1 AS ones
          UNION ALL SELECT 2, 1, 'parse', 60, 1
          UNION ALL SELECT 3, 2, 'lex', 30, 1
          UNION ALL SELECT 4, 1, 'alloc', 5, 1
          UNION ALL SELECT 5, 1, 'emit', 35, 1;

          -- Filter short calls, propagate depth, then filter depth >= 2.
          SELECT fn, depth
          FROM _tree_to_table!(
            _tree_filter(
              _tree_propagate_down(
                _tree_filter(
                  _tree_from_table!((SELECT * FROM calls), (fn, dur, ones)),
                  _tree_where(_tree_constraint('dur', '>=', 10))
                ),
                'SUM(ones) AS depth'
              ),
              _tree_where(_tree_constraint('depth', '>=', 2))
            ),
            (fn, depth)
          )
          ORDER BY fn;
        """,
        out=Csv("""
        "fn","depth"
        "emit",2
        "lex",3
        "parse",2
        """))

  def test_propagate_down_chained(self):
    """Chained propagation: use output of first propagation as source."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.propagate;

          CREATE PERFETTO TABLE calls AS
          SELECT 1 AS id, NULL AS parent_id, 'main' AS fn, 1 AS ones
          UNION ALL SELECT 2, 1, 'parse', 1
          UNION ALL SELECT 3, 2, 'lex', 1
          UNION ALL SELECT 4, 1, 'emit', 1;

          -- First propagation: SUM(ones) AS depth -> 1,2,3,2
          -- Second propagation: SUM(depth) AS cumulative_depth -> 1,3,6,3
          SELECT fn, depth, cumulative_depth
          FROM _tree_to_table!(
            _tree_propagate_down(
              _tree_propagate_down(
                _tree_from_table!((SELECT * FROM calls), (fn, ones)),
                'SUM(ones) AS depth'
              ),
              'SUM(depth) AS cumulative_depth'
            ),
            (fn, depth, cumulative_depth)
          )
          ORDER BY fn;
        """,
        out=Csv("""
        "fn","depth","cumulative_depth"
        "emit",2,3
        "lex",3,6
        "main",1,1
        "parse",2,3
        """))
