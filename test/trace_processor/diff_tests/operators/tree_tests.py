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

from python.generators.diff_tests.testing import Path, DataPath
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint, TraceInjector
from python.generators.diff_tests.testing import TestSuite

# Create a hierarchical tree structure with id/parent_id
CREATE_TEST_TABLE = """
  CREATE PERFETTO TABLE nodes AS
  WITH data(id, parent_id, name, size) AS (
    VALUES
      (1, NULL, 'root1', 100),
      (2, 1, 'child1a', 40),
      (3, 1, 'child1b', 60),
      (4, 2, 'grandchild1a1', 15),
      (5, 2, 'grandchild1a2', 25),
      (6, NULL, 'root2', 50),
      (7, 6, 'child2a', 30),
      (8, 6, 'child2b', 20)
  )
  SELECT * FROM data;
"""

CREATE_TREE_TABLE_QUERY = """
  CREATE VIRTUAL TABLE tree USING __intrinsic_tree(
    '(SELECT * FROM nodes)',
    'id',
    'parent_id'
  );
"""


class Tree(TestSuite):

  def test_tree_basic(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_TREE_TABLE_QUERY}
          SELECT name, size, __depth__, __has_children__, __child_count__ FROM tree
        """,
        out=Csv("""
"name","size","__depth__","__has_children__","__child_count__"
"root1",100,0,1,2
"root2",50,0,1,2
        """))

  def test_tree_expanded(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_TREE_TABLE_QUERY}
          SELECT name, size, __depth__, __has_children__, __child_count__ FROM tree
          WHERE __expanded_ids__ = '1'
        """,
        out=Csv("""
"name","size","__depth__","__has_children__","__child_count__"
"root1",100,0,1,2
"child1a",40,1,1,2
"child1b",60,1,0,0
"root2",50,0,1,2
        """))

  def test_tree_expanded_deep(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_TREE_TABLE_QUERY}
          SELECT name, size, __depth__, __has_children__ FROM tree
          WHERE __expanded_ids__ = '1,2'
        """,
        out=Csv("""
"name","size","__depth__","__has_children__"
"root1",100,0,1
"child1a",40,1,1
"grandchild1a1",15,2,0
"grandchild1a2",25,2,0
"child1b",60,1,0
"root2",50,0,1
        """))

  def test_tree_collapsed(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_TREE_TABLE_QUERY}
          SELECT name, size, __depth__, __has_children__ FROM tree
          WHERE __collapsed_ids__ = '1'
        """,
        out=Csv("""
"name","size","__depth__","__has_children__"
"root1",100,0,1
"root2",50,0,1
"child2a",30,1,0
"child2b",20,1,0
        """))

  def test_tree_sort_asc(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_TREE_TABLE_QUERY}
          SELECT name, size, __depth__ FROM tree
          WHERE __sort__ = 'size ASC'
        """,
        out=Csv("""
"name","size","__depth__"
"root2",50,0
"root1",100,0
        """))

  def test_tree_sort_desc(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_TREE_TABLE_QUERY}
          SELECT name, size, __depth__ FROM tree
          WHERE __sort__ = 'size DESC'
        """,
        out=Csv("""
"name","size","__depth__"
"root1",100,0
"root2",50,0
        """))

  def test_tree_sort_by_name(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_TREE_TABLE_QUERY}
          SELECT name, size, __depth__ FROM tree
          WHERE __sort__ = 'name ASC'
        """,
        out=Csv("""
"name","size","__depth__"
"root1",100,0
"root2",50,0
        """))

  def test_tree_limit(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_TREE_TABLE_QUERY}
          SELECT name, size, __depth__ FROM tree
          WHERE __expanded_ids__ = '1,6' AND __limit__ = 3
        """,
        out=Csv("""
"name","size","__depth__"
"root1",100,0
"child1a",40,1
"child1b",60,1
        """))

  def test_tree_offset_and_limit(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_TREE_TABLE_QUERY}
          SELECT name, size, __depth__ FROM tree
          WHERE __expanded_ids__ = '1,6' AND __offset__ = 2 AND __limit__ = 2
        """,
        out=Csv("""
"name","size","__depth__"
"child1b",60,1
"root2",50,0
        """))

  def test_tree_expand_all(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_TREE_TABLE_QUERY}
          SELECT name, size, __depth__, __has_children__ FROM tree
          WHERE __expanded_ids__ = '1,2,6'
        """,
        out=Csv("""
"name","size","__depth__","__has_children__"
"root1",100,0,1
"child1a",40,1,1
"grandchild1a1",15,2,0
"grandchild1a2",25,2,0
"child1b",60,1,0
"root2",50,0,1
"child2a",30,1,0
"child2b",20,1,0
        """))

  def test_tree_sort_expanded(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_TREE_TABLE_QUERY}
          SELECT name, size, __depth__ FROM tree
          WHERE __expanded_ids__ = '1,6' AND __sort__ = 'size ASC'
        """,
        out=Csv("""
"name","size","__depth__"
"root2",50,0
"child2b",20,1
"child2a",30,1
"root1",100,0
"child1a",40,1
"child1b",60,1
        """))

  def test_tree_table_name_input(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          CREATE VIRTUAL TABLE tree_direct USING __intrinsic_tree(
            'nodes',
            'id',
            'parent_id'
          );
          SELECT name, size, __depth__ FROM tree_direct
        """,
        out=Csv("""
"name","size","__depth__"
"root1",100,0
"root2",50,0
        """))

  def test_tree_offset_past_data(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_TREE_TABLE_QUERY}
          SELECT name, size, __depth__ FROM tree
          WHERE __offset__ = 100
        """,
        out=Csv("""
"name","size","__depth__"
        """))

  def test_tree_collapsed_all_expanded(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_TREE_TABLE_QUERY}
          SELECT name, size, __depth__, __has_children__ FROM tree
          WHERE __collapsed_ids__ = ''
        """,
        out=Csv("""
"name","size","__depth__","__has_children__"
"root1",100,0,1
"child1a",40,1,1
"grandchild1a1",15,2,0
"grandchild1a2",25,2,0
"child1b",60,1,0
"root2",50,0,1
"child2a",30,1,0
"child2b",20,1,0
        """))
