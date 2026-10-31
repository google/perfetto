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

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from python.generators.diff_tests.testing import TextProto


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
            is_language_runtime, is_container, is_static_class,
            obj_count, size_bytes,
            dominated_obj_count, dominated_size_bytes
          FROM android_heap_graph_class_aggregation;
        """,
        out=Csv("""
          "graph_sample_ts","upid","type_name","is_libcore_or_array","is_language_runtime","is_container","is_static_class","obj_count","size_bytes","dominated_obj_count","dominated_size_bytes"
          10,2,"A",0,0,0,0,2,200,4,11200
          10,2,"B",0,0,0,0,1,1000,1,1000
          10,2,"java.lang.String",1,1,0,0,2,10666,2,10666
        """))

  def test_heap_graph_aggregation_class_types(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            process_tree {
              processes {
                pid: 2
                ppid: 1
                cmdline: "system_server"
                uid: 1000
              }
            }
          }
          packet {
            trusted_packet_sequence_id: 999
            timestamp: 10
            [com.android.art.tracing.ArtHeapGraphTracePacket.heap_graph] {
              pid: 2
              roots {
                root_type: ROOT_JNI_GLOBAL
                object_ids: 1
              }
              objects { id: 1 type_id: 1 self_size: 10 }
              objects { id: 2 type_id: 2 self_size: 10 }
              objects { id: 3 type_id: 3 self_size: 10 }
              objects { id: 4 type_id: 4 self_size: 10 }
              objects { id: 5 type_id: 5 self_size: 10 }
              objects { id: 6 type_id: 6 self_size: 10 }
              objects { id: 7 type_id: 7 self_size: 10 }
              objects { id: 8 type_id: 8 self_size: 10 }
              objects { id: 9 type_id: 9 self_size: 10 }
              objects { id: 10 type_id: 10 self_size: 10 }
              objects { id: 11 type_id: 11 self_size: 10 }
              objects { id: 12 type_id: 12 self_size: 10 }
              objects { id: 13 type_id: 13 self_size: 10 }
              objects { id: 14 type_id: 14 self_size: 10 }
              objects { id: 15 type_id: 15 self_size: 10 }
              objects { id: 16 type_id: 16 self_size: 10 }
              objects { id: 17 type_id: 17 self_size: 10 }
              objects { id: 18 type_id: 18 self_size: 10 }
              objects { id: 19 type_id: 19 self_size: 10 }
              objects { id: 20 type_id: 20 self_size: 10 }
              objects { id: 21 type_id: 21 self_size: 10 }
              objects { id: 22 type_id: 22 self_size: 10 }
              objects { id: 23 type_id: 23 self_size: 10 }
              objects { id: 24 type_id: 24 self_size: 10 }
              objects { id: 25 type_id: 25 self_size: 10 }
              objects { id: 26 type_id: 26 self_size: 10 }
              types { id: 1 class_name: "com.example.RegularClass" }
              types { id: 2 class_name: "java.lang.String" }
              types { id: 3 class_name: "j$.time.Instant" }
              types { id: 4 class_name: "kotlin.Pair" }
              types { id: 5 class_name: "kotlinx.coroutines.JobImpl" }
              types { id: 6 class_name: "java.util.Locale" }
              types { id: 7 class_name: "java.util.HashMap" }
              types { id: 8 class_name: "java.util.HashMap$Node" }
              types { id: 9 class_name: "java.util.Arrays$ArrayList" }
              types { id: 10 class_name: "j$.util.concurrent.ConcurrentHashMap$Node" }
              types { id: 11 class_name: "android.util.ArrayMap" }
              types { id: 12 class_name: "kotlin.collections.ArrayList" }
              types { id: 13 class_name: "androidx.collection.ArraySet" }
              types { id: 14 class_name: "com.example.RegularClass[]" }
              types { id: 15 class_name: "java.lang.Class<com.example.RegularClass>" }
              types { id: 16 class_name: "java.lang.Class<java.lang.String>" }
              types { id: 17 class_name: "java.lang.Class<j$.time.Instant>" }
              types { id: 18 class_name: "java.lang.Class<java.util.HashMap>" }
              types { id: 19 class_name: "java.lang.Class<j$.util.concurrent.ConcurrentHashMap>" }
              types { id: 20 class_name: "java.lang.Class<com.example.RegularClass[]>" }
              types { id: 21 class_name: "sun.misc.Cleaner" }
              types { id: 22 class_name: "dalvik.system.PathClassLoader" }
              types { id: 23 class_name: "java.lang.Class<sun.misc.Cleaner>" }
              types { id: 24 class_name: "java.lang.Class<dalvik.system.PathClassLoader>" }
              types { id: 25 class_name: "libcore.io.BlockGuardOs" }
              types { id: 26 class_name: "java.lang.Class<libcore.io.BlockGuardOs>" }
              continued: false
              index: 0
            }
          }
        """),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_class_aggregation;

          SELECT
            type_name,
            is_libcore_or_array,
            is_language_runtime,
            is_container,
            is_static_class
          FROM android_heap_graph_class_aggregation
          ORDER BY type_id;
        """,
        out=Csv("""
          "type_name","is_libcore_or_array","is_language_runtime","is_container","is_static_class"
          "com.example.RegularClass",0,0,0,0
          "java.lang.String",1,1,0,0
          "j$.time.Instant",1,1,0,0
          "kotlin.Pair",1,1,0,0
          "kotlinx.coroutines.JobImpl",1,1,0,0
          "java.util.Locale",1,1,0,0
          "java.util.HashMap",1,1,1,0
          "java.util.HashMap$Node",1,1,1,0
          "java.util.Arrays$ArrayList",1,1,1,0
          "j$.util.concurrent.ConcurrentHashMap$Node",1,1,1,0
          "android.util.ArrayMap",1,0,1,0
          "kotlin.collections.ArrayList",1,1,1,0
          "androidx.collection.ArraySet",1,0,1,0
          "com.example.RegularClass[]",1,0,1,0
          "java.lang.Class<com.example.RegularClass>",0,0,0,1
          "java.lang.Class<java.lang.String>",1,1,0,1
          "java.lang.Class<j$.time.Instant>",1,1,0,1
          "java.lang.Class<java.util.HashMap>",1,1,1,1
          "java.lang.Class<j$.util.concurrent.ConcurrentHashMap>",1,1,1,1
          "java.lang.Class<com.example.RegularClass[]>",1,0,1,1
          "sun.misc.Cleaner",1,1,0,0
          "dalvik.system.PathClassLoader",1,1,0,0
          "java.lang.Class<sun.misc.Cleaner>",1,1,0,1
          "java.lang.Class<dalvik.system.PathClassLoader>",1,1,0,1
          "libcore.io.BlockGuardOs",1,1,0,0
          "java.lang.Class<libcore.io.BlockGuardOs>",1,1,0,1
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

  def test_heap_graph_class_tree_native_size(self):
    return DiffTestBlueprint(
        trace=Path('../../parser/profiling/heap_graph_native_size.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.class_tree;

          SELECT p.name AS parent_name, c.name, c.heap_type, c.self_count, c.self_size
          FROM _heap_graph_class_tree c
          LEFT JOIN _heap_graph_class_tree p ON c.parent_id = p.id
          WHERE c.name GLOB '*Bitmap*'
          ORDER BY c.name;
        """,
        out=Csv("""
          "parent_name","name","heap_type","self_count","self_size"
          "android.graphics.Bitmap","[native] android.graphics.Bitmap","HEAP_TYPE_NATIVE",1,123456
          "[NULL]","android.graphics.Bitmap","[NULL]",1,46
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

  def test_heap_graph_binder_stub(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_binder_stubs.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.binder;

          SELECT
            id,
            class_name,
            interface_name,
            is_remote_held,
            is_untracked,
            remote_only_obj_count,
            remote_only_size_bytes,
            dominated_size_bytes
          FROM android_heap_graph_binder_stub;
        """,
        out=Csv("""
          "id","class_name","interface_name","is_remote_held","is_untracked","remote_only_obj_count","remote_only_size_bytes","dominated_size_bytes"
          1,"com.example.FooService$1","com.example.IFoo$Stub",1,0,0,0,132
          4,"com.example.FooService$1","com.example.IFoo$Stub",1,1,3,632,232
          7,"com.example.BarRecord","com.example.BarRecord",1,1,2,114,114
        """))

  def test_heap_graph_binder_stub_summary(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_for_binder_stubs.textproto'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.binder;

          SELECT
            class_name,
            interface_name,
            instance_count,
            untracked_count,
            remote_only_obj_count,
            remote_only_size_bytes,
            dominated_size_bytes,
            max_instance_dominated_bytes,
            example_id,
            is_single_instance
          FROM android_heap_graph_binder_stub_summary;
        """,
        out=Csv("""
          "class_name","interface_name","instance_count","untracked_count","remote_only_obj_count","remote_only_size_bytes","dominated_size_bytes","max_instance_dominated_bytes","example_id","is_single_instance"
          "com.example.FooService$1","com.example.IFoo$Stub",2,1,3,632,364,232,4,0
          "com.example.BarRecord","com.example.BarRecord",1,1,2,114,114,114,7,1
        """))

  def test_heap_graph_binder_stub_summary_system_server(self):
    return DiffTestBlueprint(
        trace=DataPath('system-server-heap-graph-new.pftrace'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_graph.binder;

          SELECT
            class_name,
            interface_name,
            instance_count,
            untracked_count,
            remote_only_obj_count,
            remote_only_size_bytes,
            dominated_size_bytes,
            max_instance_dominated_bytes,
            example_id IS NOT NULL AS has_example_id,
            is_single_instance
          FROM android_heap_graph_binder_stub_summary
          LIMIT 5;
        """,
        out=Csv("""
          "class_name","interface_name","instance_count","untracked_count","remote_only_obj_count","remote_only_size_bytes","dominated_size_bytes","max_instance_dominated_bytes","has_example_id","is_single_instance"
          "com.android.server.am.ServiceRecord","com.android.server.am.ServiceRecord",131,44,767,51631,156684,5069,1,0
          "com.android.server.am.PendingIntentRecord","android.content.IIntentSender$Stub",51,40,165,6160,7799,349,1,0
          "com.android.server.am.ContentProviderConnection","com.android.server.am.ContentProviderConnection",56,22,70,5439,7018,1203,1,0
          "com.android.server.notification.NotificationManagerService$StatusBarNotificationHolder","android.service.notification.IStatusBarNotificationHolder$Stub",91,91,96,3136,2548,28,1,0
          "com.android.server.media.MediaSessionRecord$ControllerStub","android.media.session.ISessionController$Stub",3,3,72,2663,2084,720,1,0
        """))
