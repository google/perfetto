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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class HeapGraphDominatorTree(TestSuite):

  def test_heap_graph_dominator_tree(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_dominator_tree.textproto'),
        query="""
          INCLUDE PERFETTO MODULE memory.heap_graph_dominator_tree;

          SELECT
            node.id,
            node.idom_id,
            node.dominated_obj_count,
            node.dominated_size_bytes,
            cls.name AS type_name
          FROM memory_heap_graph_dominator_tree node
          JOIN heap_graph_object obj USING(id)
          JOIN heap_graph_class cls ON obj.type_id = cls.id
          ORDER BY type_name;
        """,
        out=Csv("""
          "id","idom_id","dominated_obj_count","dominated_size_bytes",\
"type_name"
          0,12,1,3,"A"
          2,12,1,3,"B"
          4,12,4,12,"C"
          1,12,2,6,"D"
          3,12,1,3,"E"
          5,4,1,3,"F"
          6,4,2,6,"G"
          8,12,1,3,"H"
          9,12,1,3,"I"
          10,6,1,3,"J"
          11,12,1,3,"K"
          7,1,1,3,"L"
          13,22,6,922,"M"
          16,22,3,100,"N"
          14,13,4,904,"O"
          15,13,1,16,"P"
          17,16,1,32,"Q"
          12,25,13,39,"R"
          22,25,10,1023,"S"
          18,16,1,64,"T"
          19,14,1,128,"U"
          20,14,1,256,"V"
          21,14,1,512,"W"
          23,25,1,1024,"java.lang.ref.FinalizerReference"
        """))
