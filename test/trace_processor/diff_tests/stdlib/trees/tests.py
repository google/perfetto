#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Trees(TestSuite):

  def test_tree_from_table_simple(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        INCLUDE PERFETTO MODULE std.trees.table_conversion;

        CREATE PERFETTO TABLE test_tree AS
        SELECT 0 AS id, NULL AS parent_id, 'root' AS name, 100 AS value
        UNION ALL
        SELECT 1, 0, 'child1', 200
        UNION ALL
        SELECT 2, 0, 'child2', 300
        UNION ALL
        SELECT 3, 1, 'grandchild', 400;

        SELECT _tree_id, _tree_parent_id, name, value
        FROM tree_to_table!(
          tree_from_table!((SELECT id, parent_id, name, value FROM test_tree), (name, value)),
          (name, value)
        )
        ORDER BY _tree_id;
      """,
        out=Csv("""
        "_tree_id","_tree_parent_id","name","value"
        0,"[NULL]","root",100
        1,0,"child1",200
        2,0,"child2",300
        3,1,"grandchild",400
      """))

  def test_tree_from_table_multiple_roots(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        INCLUDE PERFETTO MODULE std.trees.table_conversion;

        CREATE PERFETTO TABLE test_tree AS
        SELECT 0 AS id, NULL AS parent_id, 'root1' AS name
        UNION ALL
        SELECT 1, NULL, 'root2'
        UNION ALL
        SELECT 2, 0, 'child_of_root1'
        UNION ALL
        SELECT 3, 1, 'child_of_root2';

        SELECT _tree_id, _tree_parent_id, name
        FROM tree_to_table!(
          tree_from_table!((SELECT id, parent_id, name FROM test_tree), (name)),
          (name)
        )
        ORDER BY _tree_id;
      """,
        out=Csv("""
        "_tree_id","_tree_parent_id","name"
        0,"[NULL]","root1"
        1,"[NULL]","root2"
        2,0,"child_of_root1"
        3,1,"child_of_root2"
      """))

  def test_tree_preserves_data_types(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        INCLUDE PERFETTO MODULE std.trees.table_conversion;

        CREATE PERFETTO TABLE test_tree AS
        SELECT 0 AS id, NULL AS parent_id, 'text' AS str_col, 42 AS int_col, 3.14 AS float_col
        UNION ALL
        SELECT 1, 0, 'child', 100, 2.71;

        SELECT _tree_id, _tree_parent_id, str_col, int_col, float_col
        FROM tree_to_table!(
          tree_from_table!((SELECT id, parent_id, str_col, int_col, float_col FROM test_tree), (str_col, int_col, float_col)),
          (str_col, int_col, float_col)
        )
        ORDER BY _tree_id;
      """,
        out=Csv("""
        "_tree_id","_tree_parent_id","str_col","int_col","float_col"
        0,"[NULL]","text",42,3.140000
        1,0,"child",100,2.710000
      """))

  def test_tree_deep_hierarchy(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        INCLUDE PERFETTO MODULE std.trees.table_conversion;

        CREATE PERFETTO TABLE test_tree AS
        SELECT 0 AS id, NULL AS parent_id, 'level0' AS name
        UNION ALL
        SELECT 1, 0, 'level1'
        UNION ALL
        SELECT 2, 1, 'level2'
        UNION ALL
        SELECT 3, 2, 'level3'
        UNION ALL
        SELECT 4, 3, 'level4';

        SELECT _tree_id, _tree_parent_id, name
        FROM tree_to_table!(
          tree_from_table!((SELECT id, parent_id, name FROM test_tree), (name)),
          (name)
        )
        ORDER BY _tree_id;
      """,
        out=Csv("""
        "_tree_id","_tree_parent_id","name"
        0,"[NULL]","level0"
        1,0,"level1"
        2,1,"level2"
        3,2,"level3"
        4,3,"level4"
      """))

  def test_tree_single_node(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        INCLUDE PERFETTO MODULE std.trees.table_conversion;

        CREATE PERFETTO TABLE test_tree AS
        SELECT 0 AS id, NULL AS parent_id, 'only_node' AS name;

        SELECT _tree_id, _tree_parent_id, name
        FROM tree_to_table!(
          tree_from_table!((SELECT id, parent_id, name FROM test_tree), (name)),
          (name)
        )
        ORDER BY _tree_id;
      """,
        out=Csv("""
        "_tree_id","_tree_parent_id","name"
        0,"[NULL]","only_node"
      """))
