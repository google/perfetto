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


class TreeFilter(TestSuite):
  """Tests for tree filtering with reparenting behavior."""

  def test_filter_leaf_nodes(self):
    """Filtering leaf nodes should just remove them."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.filter;

          CREATE PERFETTO TABLE input_tree AS
          SELECT 1 AS id, NULL AS parent_id, 'root' AS name
          UNION ALL SELECT 2, 1, 'keep'
          UNION ALL SELECT 3, 1, 'remove'
          UNION ALL SELECT 4, 2, 'child';

          -- Composition: table → tree → filter (constraint-based) → table
          SELECT _tree_id, _tree_parent_id, id, name
          FROM _tree_to_table!(
            _tree_filter(
              _tree_from_table!((SELECT * FROM input_tree), (name)),
              _tree_where(_tree_constraint('name', '!=', 'remove'))
            ),
            (name)
          )
          ORDER BY id;
        """,
        out=Csv("""
        "_tree_id","_tree_parent_id","id","name"
        0,"[NULL]",1,"root"
        1,0,2,"keep"
        2,1,4,"child"
        """))

  def test_filter_intermediate_with_reparenting(self):
    """Filtering intermediate nodes should reparent children to grandparent."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.filter;

          CREATE PERFETTO TABLE input_tree AS
          SELECT 1 AS id, NULL AS parent_id, 1 AS depth
          UNION ALL SELECT 2, 1, 2   -- Will be filtered
          UNION ALL SELECT 3, 2, 3   -- Should be reparented to node 1
          UNION ALL SELECT 4, 1, 2;  -- Sibling, unaffected

          SELECT _tree_id, _tree_parent_id, id, depth
          FROM _tree_to_table!(
            _tree_filter(
              _tree_from_table!((SELECT * FROM input_tree), (depth)),
              _tree_where(_tree_constraint('depth', '!=', 2))
            ),
            (depth)
          )
          ORDER BY id;
        """,
        out=Csv("""
        "_tree_id","_tree_parent_id","id","depth"
        0,"[NULL]",1,1
        1,0,3,3
        """))

  def test_filter_all_nodes(self):
    """Filtering all nodes should result in empty tree."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.filter;

          CREATE PERFETTO TABLE input_tree AS
          SELECT 1 AS id, NULL AS parent_id, 'a' AS tag
          UNION ALL SELECT 2, 1, 'b'
          UNION ALL SELECT 3, 2, 'c';

          SELECT _tree_id, _tree_parent_id, id, tag
          FROM _tree_to_table!(
            _tree_filter(
              _tree_from_table!((SELECT * FROM input_tree), (tag)),
              _tree_where(_tree_constraint('tag', '=', 'nonexistent'))
            ),
            (tag)
          )
          ORDER BY id;
        """,
        out=Csv("""
        "_tree_id","_tree_parent_id","id","tag"
        """))

  def test_filter_root_nodes(self):
    """Filtering root nodes promotes children to new roots."""
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.filter;

          CREATE PERFETTO TABLE input_tree AS
          SELECT 1 AS id, NULL AS parent_id, 0 AS level
          UNION ALL SELECT 2, 1, 1          -- Child of root 1, will become new root
          UNION ALL SELECT 3, NULL, 0       -- Another root (kept)
          UNION ALL SELECT 4, 3, 1;         -- Child of root 3 (kept)

          SELECT _tree_id, _tree_parent_id, id, level
          FROM _tree_to_table!(
            _tree_filter(
              _tree_from_table!((SELECT * FROM input_tree), (level)),
              _tree_where(_tree_constraint('id', '!=', 1))  -- Filter out root 1
            ),
            (level)
          )
          ORDER BY id;
        """,
        out=Csv("""
        "_tree_id","_tree_parent_id","id","level"
        0,"[NULL]",2,1
        1,"[NULL]",3,0
        2,1,4,1
        """))
