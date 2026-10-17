#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class HeapGraph(TestSuite):

  def test_heap_graph_dominator_tree(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_dominator_tree.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;

          SELECT
            node.id,
            node.idom_id,
            node.dominated_obj_count,
            node.dominated_size_bytes,
            node.depth,
            cls.name AS type_name
          FROM heap_graph_dominator_tree node
          JOIN heap_graph_object obj USING(id)
          JOIN heap_graph_class cls ON obj.type_id = cls.id
          ORDER BY type_name;
        """,
        out=Csv("""
          "id","idom_id","dominated_obj_count","dominated_size_bytes","depth","type_name"
          0,12,1,3,2,"A"
          2,12,1,3,2,"B"
          4,12,4,12,2,"C"
          1,12,2,6,2,"D"
          3,12,1,3,2,"E"
          5,4,1,3,3,"F"
          6,4,2,6,3,"G"
          8,12,1,3,2,"H"
          9,12,1,3,2,"I"
          10,6,1,3,4,"J"
          11,12,1,3,2,"K"
          7,1,1,3,3,"L"
          13,22,6,922,2,"M"
          16,22,3,100,2,"N"
          14,13,4,904,3,"O"
          15,13,1,16,3,"P"
          17,16,1,32,3,"Q"
          12,"[NULL]",13,39,1,"R"
          22,"[NULL]",10,1023,1,"S"
          18,16,1,64,3,"T"
          19,14,1,128,4,"U"
          20,14,1,256,4,"V"
          21,14,1,512,4,"W"
          23,"[NULL]",1,1024,1,"sun.misc.Cleaner"
        """))

  def test_heap_graph_aggregation(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_aggregation.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_class_aggregation;

          SELECT graph_sample_ts, upid, type_name, is_libcore_or_array,
            obj_count, size_bytes,
            dominated_obj_count, dominated_size_bytes
          FROM android_heap_graph_class_aggregation;
        """,
        out=Csv("""
          "graph_sample_ts","upid","type_name","is_libcore_or_array","obj_count","size_bytes","dominated_obj_count","dominated_size_bytes"
          10,2,"A",0,2,200,4,11200
          10,2,"B",0,1,1000,1,1000
          10,2,"java.lang.String",1,2,10666,2,10666
        """))

  def test_heap_graph_class_summary_tree(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_aggregation.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.class_summary_tree;

          SELECT name, self_count, self_size, cumulative_count, cumulative_size
          FROM android_heap_graph_class_summary_tree
          ORDER BY cumulative_size DESC;
        """,
        out=Csv("""
          "name","self_count","self_size","cumulative_count","cumulative_size"
          "A",2,200,4,11200
          "java.lang.String",1,10000,1,10000
          "B",1,1000,1,1000
          "java.lang.String",1,666,1,666
        """))

  # The same class loaded by two class loaders has the same name but distinct
  # type_ids. The class tree keys on class name, so the two instances must
  # coalesce into a single node (self_count = 2) rather than splitting.
  def test_heap_graph_class_tree_dedups_class_loaders(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_class_loader_dedup.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.class_tree;

          SELECT p.name AS parent_name, c.name, c.self_count, c.self_size
          FROM _heap_graph_class_tree c
          LEFT JOIN _heap_graph_class_tree p ON c.parent_id = p.id
          ORDER BY c.name, parent_name;
        """,
        out=Csv("""
          "parent_name","name","self_count","self_size"
          "Root","Clock",2,32
          "[NULL]","Root",1,8
        """))

  def test_heap_graph_dominator_class_tree_dedups_class_loaders(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_class_loader_dedup.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_class_tree;

          SELECT p.name AS parent_name, c.name, c.self_count, c.self_size
          FROM _heap_graph_dominator_class_tree c
          LEFT JOIN _heap_graph_dominator_class_tree p ON c.parent_id = p.id
          ORDER BY c.name, parent_name;
        """,
        out=Csv("""
          "parent_name","name","self_count","self_size"
          "Root","Clock",2,32
          "[NULL]","Root",1,8
        """))

  def test_heap_graph_stats(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_aggregation.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_stats;

          SELECT *
          FROM android_heap_graph_stats
          ORDER BY upid DESC;
        """,
        out=Csv("""
          "upid","graph_sample_ts","process_uptime","total_heap_size","total_native_alloc_registry_size","total_obj_count","reachable_heap_size","reachable_native_alloc_registry_size","reachable_obj_count","oom_score_adj","anon_rss_and_swap_size","dmabuf_rss_size","art_bytes_allocated","is_out_of_memory_error","oome_failing_allocation_size","oome_remaining_art_free_bytes"
          2,10,"[NULL]",11866,0,5,11866,0,5,-900,4096000,8192000,"[NULL]","[NULL]","[NULL]","[NULL]"
        """))

  def test_heap_graph_stats_oome(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_oome.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_stats;

          SELECT *
          FROM android_heap_graph_stats
          ORDER BY upid DESC;
        """,
        out=Csv("""
          "upid","graph_sample_ts","process_uptime","total_heap_size","total_native_alloc_registry_size","total_obj_count","reachable_heap_size","reachable_native_alloc_registry_size","reachable_obj_count","oom_score_adj","anon_rss_and_swap_size","dmabuf_rss_size","art_bytes_allocated","is_out_of_memory_error","oome_failing_allocation_size","oome_remaining_art_free_bytes"
          2,10,"[NULL]",11866,0,5,11866,0,5,-900,4096000,8192000,1234,1,1000,5000
        """))

  def test_heap_graph_collections(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_collections.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.collections;

          SELECT
            class_name,
            kind,
            layout,
            element_count,
            capacity,
            overhead_size,
            retained_size,
            field_name,
            element_class,
            distinct_element_class_count
          FROM _heap_graph_collection
          ORDER BY field_name;
        """,
        out=Csv("""
          "class_name","kind","layout","element_count","capacity","overhead_size","retained_size","field_name","element_class","distinct_element_class_count"
          "java.lang.Object[]","array","array",0,0,12,12,"[NULL]","[NULL]",0
          "java.util.ArrayList","list","slots",1,1,24,24,"[NULL]","java.util.ArrayList",1
          "java.util.ArrayList","list","slots",1,1,24,24,"[NULL]","java.util.ArrayList",1
          "java.util.ArrayList","list","slots",1,1,40,56,"[NULL]","Elem",1
          "java.util.ArrayList","list","slots",2,2,44,76,"Holder.items","Elem",1
          "android.util.ArrayMap","map","slot_pairs",2,2,80,128,"Root.arrayMap","Elem",1
          "java.util.LinkedHashMap","map","nodes",1,1,112,112,"Root.linkedMap","Elem",1
          "java.util.ArrayList","list","slots",1,3,48,64,"Root.list","Elem",1
          "java.util.ImmutableCollections$List12","list","fields",2,"[NULL]",24,56,"Root.list12","Elem",1
          "android.util.LruCache","map","delegate",1,1,144,144,"Root.lruCache","Elem",1
          "java.util.HashMap","map","nodes",2,2,132,164,"Root.map","Elem",1
          "java.util.HashMap","map","nodes",1,1,96,168,"Root.mapOfLists","java.util.ArrayList",2
          "java.util.ArrayList","list","slots",1,1,40,56,"Root.mapOfLists[value]","Elem",1
          "java.util.ArrayList","list","slots",2,2,44,192,"Root.nested","java.util.ArrayList",2
          "java.util.ArrayList","list","slots",1,1,40,56,"Root.nested[]","Elem",1
          "java.util.ArrayList","list","slots",1,1,40,56,"Root.otherList","Elem",1
          "java.util.concurrent.ConcurrentLinkedQueue","queue","nodes",1,"[NULL]",72,88,"Root.queue","Elem",1
          "Foo[]","array","array",1,1,16,32,"Root.rawArray","Elem",1
          "java.util.HashSet","set","delegate",1,1,112,136,"Root.set","Elem",1
          "java.util.Collections$UnmodifiableCollection","collection","delegate",1,1,16,16,"Root.sharedDelegate","Elem",1
          "android.util.SparseArray","map","slots",2,3,72,104,"Root.sparseArray","Elem",1
          "CustomHashMapSubclass","map","nodes",1,1,96,96,"Root.subMap","Elem",1
          "java.util.ArrayList","list","slots",1,1,40,168,"Root.tree","TreeNode",1
          "UnsupportedCollection","collection","unsupported","[NULL]","[NULL]","[NULL]",32,"Root.unsupported","[NULL]","[NULL]"
          "java.util.ArrayList","list","slots",1,1,40,112,"TreeNode.children","TreeNode",1
          "java.util.ArrayList","list","slots",1,1,40,56,"TreeNode.children","Elem",1
        """))

  def test_heap_graph_collections_unsupported(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_collections.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.collections;

          SELECT
            class_name,
            instance_count,
            retained_size_sum,
            reachable_size_sum
          FROM _heap_graph_collection_unsupported_class
          ORDER BY class_name;
        """,
        out=Csv("""
          "class_name","instance_count","retained_size_sum","reachable_size_sum"
          "UnsupportedCollection",1,32,32
        """))

  def test_heap_graph_collection_tree(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_collections.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.collection_tree;

          SELECT
            t.name,
            p.name AS parent_name,
            t.self_count,
            t.self_size,
            t.self_element_count
          FROM _heap_graph_collection_tree AS t
          LEFT JOIN _heap_graph_collection_tree AS p
            ON p.id = t.parent_id
          ORDER BY t.name;
        """,
        out=Csv("""
          "name","parent_name","self_count","self_size","self_element_count"
          "Root","[NULL]",0,0,0
          "arrayMap: android.util.ArrayMap<Elem>","Root",1,128,2
          "java.lang.Object[]","[NULL]",1,12,0
          "java.lang.Object[]","[NULL]",0,0,0
          "java.util.ArrayList<ArrayList>","[NULL]",2,48,2
          "java.util.ArrayList<Elem>","java.lang.Object[]",1,56,1
          "linkedMap: java.util.LinkedHashMap<Elem>","Root",1,112,1
          "list12: java.util.ImmutableCollections$List12<Elem>","Root",1,56,2
          "list: java.util.ArrayList<Elem>","Root",1,64,1
          "lruCache: android.util.LruCache<Elem>","Root",1,144,1
          "map: java.util.HashMap<Elem>","Root",1,164,2
          "mapOfLists: java.util.HashMap<ArrayList>","Root",1,168,1
          "nested: java.util.ArrayList<ArrayList>","Root",1,192,2
          "otherList: java.util.ArrayList<Elem>","Root",1,56,1
          "queue: java.util.concurrent.ConcurrentLinkedQueue<Elem>","Root",1,88,1
          "rawArray: Foo[]","Root",1,32,1
          "set: java.util.HashSet<Elem>","Root",1,136,1
          "sharedDelegate: java.util.Collections$UnmodifiableCollection<Elem>","Root",1,16,1
          "sparseArray: android.util.SparseArray<Elem>","Root",1,104,2
          "subMap: CustomHashMapSubclass<Elem>","Root",1,96,1
          "tree: java.util.ArrayList<TreeNode>","Root",1,168,1
          "unsupported: UnsupportedCollection","Root",1,32,0
        """))

  def test_heap_graph_collection_summary(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_collections.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.collections;

          SELECT
            class_name,
            count() AS field_count,
            sum(instance_count) AS instance_count,
            sum(element_count_sum) AS element_count,
            max(element_count_max) AS element_count_max,
            sum(retained_size_sum) AS retained_size
          FROM _heap_graph_collection_summary!(
            (SELECT * FROM _heap_graph_collection WHERE class_name GLOB 'java.util.*')
          )
          GROUP BY class_name
          ORDER BY class_name;
        """,
        out=Csv("""
          "class_name","field_count","instance_count","element_count","element_count_max","retained_size"
          "java.util.ArrayList",9,12,14,2,884
          "java.util.Collections$UnmodifiableCollection",1,1,1,1,16
          "java.util.HashMap",2,2,3,2,332
          "java.util.HashSet",1,1,1,1,136
          "java.util.ImmutableCollections$List12",1,1,2,2,56
          "java.util.LinkedHashMap",1,1,1,1,112
          "java.util.concurrent.ConcurrentLinkedQueue",1,1,1,1,88
        """))

  def test_heap_graph_collections_under_frames(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_collections.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.collection_tree;

          SELECT c.field_name
          FROM _heap_graph_collections_under_frames!((
            SELECT path_hash_stable AS path_hash
            FROM _heap_graph_collection_tree
            WHERE name GLOB 'nested: *' OR name GLOB 'set: *'
          )) AS u
          JOIN _heap_graph_collection AS c USING (id)
          ORDER BY c.field_name;
        """,
        out=Csv("""
          "field_name"
          "Holder.items"
          "Root.nested"
          "Root.nested[]"
          "Root.set"
        """))

  def test_heap_graph_collection_summary_nested_in_group(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_collections.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.collections;

          SELECT
            field_name,
            instance_count,
            element_count_sum,
            retained_size_sum
          FROM _heap_graph_collection_field_summary
          WHERE field_name GLOB 'TreeNode.*' OR field_name GLOB 'Root.tree'
          ORDER BY field_name;
        """,
        out=Csv("""
          "field_name","instance_count","element_count_sum","retained_size_sum"
          "Root.tree",1,1,168
          "TreeNode.children",2,2,112
        """))

  def test_heap_graph_collection_tree_frames_of_same_name(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_collections.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.collection_tree;

          SELECT
            t.collection_class,
            t.self_count,
            (
              SELECT group_concat(c.name)
              FROM _heap_graph_collection_tree AS c
              WHERE c.parent_id = t.id
            ) AS children
          FROM _heap_graph_collection_tree AS t
          WHERE t.name = 'java.lang.Object[]' AND t.parent_id IS NULL
          ORDER BY t.collection_class;
        """,
        out=Csv("""
          "collection_class","self_count","children"
          "[NULL]",0,"java.util.ArrayList<Elem>"
          "java.lang.Object[]",1,"[NULL]"
        """))

  def test_heap_graph_collection_tree_sizes_add_up(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_collections.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.collection_tree;

          SELECT
            (SELECT sum(self_size) FROM _heap_graph_collection_tree) AS tree,
            (
              SELECT sum(retained_size)
              FROM _heap_graph_collection
              WHERE id NOT IN (SELECT id FROM _heap_graph_collection_outer)
            ) AS outermost_collections;
        """,
        out=Csv("""
          "tree","outermost_collections"
          1872,1872
        """))
