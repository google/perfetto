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


class TreeRoundtrip(TestSuite):

  def test_basic(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;

          CREATE PERFETTO TABLE input_tree AS
          SELECT 1 AS id, NULL AS parent_id, 0 AS depth
          UNION ALL SELECT 2, 1, 1
          UNION ALL SELECT 3, 1, 1
          UNION ALL SELECT 4, 2, 2;

          SELECT _tree_id, _tree_parent_id, id, parent_id, depth
          FROM _tree_to_table!(
            _tree_from_table!((SELECT * FROM input_tree), (depth)),
            (depth)
          )
          ORDER BY id;
        """,
        out=Csv("""
        "_tree_id","_tree_parent_id","id","parent_id","depth"
        0,"[NULL]",1,"[NULL]",0
        1,0,2,1,1
        2,0,3,1,1
        3,1,4,2,2
        """))

  def test_string_column(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;

          CREATE PERFETTO TABLE input_tree AS
          SELECT 1 AS id, NULL AS parent_id, 'root' AS name
          UNION ALL SELECT 2, 1, 'child1'
          UNION ALL SELECT 3, 1, 'child2'
          UNION ALL SELECT 4, 2, 'grandchild';

          SELECT _tree_id, _tree_parent_id, id, parent_id, name
          FROM _tree_to_table!(
            _tree_from_table!((SELECT * FROM input_tree), (name)),
            (name)
          )
          ORDER BY id;
        """,
        out=Csv("""
        "_tree_id","_tree_parent_id","id","parent_id","name"
        0,"[NULL]",1,"[NULL]","root"
        1,0,2,1,"child1"
        2,0,3,1,"child2"
        3,1,4,2,"grandchild"
        """))

  def test_forest(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;

          CREATE PERFETTO TABLE input_tree AS
          SELECT 1 AS id, NULL AS parent_id, 'tree1' AS label
          UNION ALL SELECT 2, NULL, 'tree2'
          UNION ALL SELECT 3, 1, 'child1'
          UNION ALL SELECT 4, 2, 'child2';

          SELECT _tree_id, _tree_parent_id, id, parent_id, label
          FROM _tree_to_table!(
            _tree_from_table!((SELECT * FROM input_tree), (label)),
            (label)
          )
          ORDER BY id;
        """,
        out=Csv("""
        "_tree_id","_tree_parent_id","id","parent_id","label"
        0,"[NULL]",1,"[NULL]","tree1"
        1,"[NULL]",2,"[NULL]","tree2"
        2,0,3,1,"child1"
        3,1,4,2,"child2"
        """))
