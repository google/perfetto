#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ProfilingHeapGraph(TestSuite):

  def test_heap_graph_flamegraph(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_baseapk.textproto'),
        query="""
        SELECT
          id,
          depth,
          name,
          map_name,
          count,
          cumulative_count,
          size,
          cumulative_size,
          parent_id
        FROM experimental_flamegraph(
          'graph',
          (SELECT max(graph_sample_ts) FROM heap_graph_object),
          NULL,
          (SELECT max(upid) FROM heap_graph_object),
          NULL,
          NULL
        )
        LIMIT 10;
        """,
        out=Path('heap_graph_flamegraph.out'))

  def test_heap_graph_object(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_baseapk.textproto'),
        query="""
        SELECT o.id,
               o.type,
               o.upid,
               o.graph_sample_ts,
               o.self_size,
               o.reference_set_id,
               o.reachable,
               c.name AS type_name,
               c.deobfuscated_name AS deobfuscated_type_name,
               o.root_type
        FROM heap_graph_object o JOIN heap_graph_class c ON o.type_id = c.id;
        """,
        out=Path('heap_graph_object.out'))

  def test_heap_graph_reference(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_baseapk.textproto'),
        query="""
        SELECT * FROM heap_graph_reference;
        """,
        out=Path('heap_graph_reference.out'))

  def test_heap_graph_object_2(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_deobfuscate_pkg.textproto'),
        query="""
        SELECT o.id,
               o.type,
               o.upid,
               o.graph_sample_ts,
               o.self_size,
               o.reference_set_id,
               o.reachable,
               c.name AS type_name,
               c.deobfuscated_name AS deobfuscated_type_name,
               o.root_type
        FROM heap_graph_object o JOIN heap_graph_class c ON o.type_id = c.id;
        """,
        out=Path('heap_graph_object.out'))

  def test_heap_graph_duplicate_flamegraph(self):
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
          timestamp: 10
          process_stats {
            processes {
              pid: 2
              rss_anon_kb: 1000
              vm_swap_kb: 3000
              oom_score_adj: 0
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 999
          timestamp: 10
          heap_graph {
            pid: 2
            types {
              id: 1
              class_name: "FactoryProducerDelegateImplActor"
              location_id: 1
            }
            roots {
              root_type: ROOT_JAVA_FRAME
              object_ids: 0x01
              object_ids: 0x01
            }
            objects {
              id: 0x01
              type_id: 1
              self_size: 64
            }
            continued: false
            index: 0
          }
        }
        """),
        query="""
        SELECT
          id,
          depth,
          name,
          map_name,
          count,
          cumulative_count,
          size,
          cumulative_size,
          parent_id
        FROM experimental_flamegraph(
          'graph',
          (SELECT max(graph_sample_ts) FROM heap_graph_object),
          NULL,
          (SELECT max(upid) FROM heap_graph_object),
          NULL,
          NULL
        )
        LIMIT 10;
        """,
        out=Path('heap_graph_duplicate_flamegraph.out'))

  def test_heap_graph_flamegraph_2(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph.textproto'),
        query="""
        SELECT
          id,
          depth,
          name,
          map_name,
          count,
          cumulative_count,
          size,
          cumulative_size,
          parent_id
        FROM experimental_flamegraph(
          'graph',
          (SELECT max(graph_sample_ts) FROM heap_graph_object),
          NULL,
          (SELECT max(upid) FROM heap_graph_object),
          NULL,
          NULL
        )
        LIMIT 10;
        """,
        out=Path('heap_graph_flamegraph.out'))

  def test_heap_graph_object_3(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph.textproto'),
        query="""
        SELECT o.id,
               o.type,
               o.upid,
               o.graph_sample_ts,
               o.self_size,
               o.reference_set_id,
               o.reachable,
               c.name AS type_name,
               c.deobfuscated_name AS deobfuscated_type_name,
               o.root_type
        FROM heap_graph_object o JOIN heap_graph_class c ON o.type_id = c.id;
        """,
        out=Path('heap_graph_object.out'))

  def test_heap_graph_object_reference_set_id(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph.textproto'),
        query="""
        SELECT o.reference_set_id
        FROM heap_graph_object o
        WHERE o.reference_set_id = 3
        """,
        out=Csv('''
          "reference_set_id"
          3
        '''))

  def test_heap_graph_reference_2(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph.textproto'),
        query="""
        SELECT * FROM heap_graph_reference;
        """,
        out=Path('heap_graph_reference.out'))

  def test_heap_graph_two_locations(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_two_locations.textproto'),
        query="""
        SELECT o.id,
               o.type,
               o.upid,
               o.graph_sample_ts,
               o.self_size,
               o.reference_set_id,
               o.reachable,
               c.name AS type_name,
               c.deobfuscated_name AS deobfuscated_type_name,
               o.root_type
        FROM heap_graph_object o JOIN heap_graph_class c ON o.type_id = c.id;
        """,
        out=Path('heap_graph_two_locations.out'))

  def test_heap_graph_object_4(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_legacy.textproto'),
        query="""
        SELECT o.id,
               o.type,
               o.upid,
               o.graph_sample_ts,
               o.self_size,
               o.reference_set_id,
               o.reachable,
               c.name AS type_name,
               c.deobfuscated_name AS deobfuscated_type_name,
               o.root_type
        FROM heap_graph_object o JOIN heap_graph_class c ON o.type_id = c.id;
        """,
        out=Path('heap_graph_object.out'))

  def test_heap_graph_reference_3(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_legacy.textproto'),
        query="""
        SELECT * FROM heap_graph_reference;
        """,
        out=Path('heap_graph_reference.out'))

  def test_heap_graph_interleaved_object(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_interleaved.textproto'),
        query="""
        SELECT o.id,
               o.type,
               o.upid,
               o.graph_sample_ts,
               o.self_size,
               o.reference_set_id,
               o.reachable,
               c.name AS type_name,
               c.deobfuscated_name AS deobfuscated_type_name,
               o.root_type
        FROM heap_graph_object o JOIN heap_graph_class c ON o.type_id = c.id;
        """,
        out=Path('heap_graph_interleaved_object.out'))

  def test_heap_graph_interleaved_reference(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_interleaved.textproto'),
        query="""
        SELECT * FROM heap_graph_reference;
        """,
        out=Path('heap_graph_interleaved_reference.out'))

  def test_heap_graph_flamegraph_system_server_heap_graph(self):
    return DiffTestBlueprint(
        trace=DataPath('system-server-heap-graph-new.pftrace'),
        query="""
        SELECT
          id,
          depth,
          name,
          map_name,
          count,
          cumulative_count,
          size,
          cumulative_size,
          parent_id
        FROM experimental_flamegraph(
          'graph',
          (SELECT max(graph_sample_ts) FROM heap_graph_object),
          NULL,
          (SELECT max(upid) FROM heap_graph_object),
          NULL,
          NULL
        )
        LIMIT 10;
        """,
        out=Path('heap_graph_flamegraph_system-server-heap-graph.out'))

  def test_heap_profile_flamegraph_system_server_native_profile(self):
    return DiffTestBlueprint(
        trace=DataPath('system-server-native-profile'),
        query="""
        SELECT *
        FROM experimental_flamegraph(
          'native',
          605908369259172,
          NULL,
          1,
          NULL,
          NULL
        )
        LIMIT 10;
        """,
        out=Csv('''
          "id","type","ts","depth","name","map_name","count","cumulative_count","size","cumulative_size","alloc_count","cumulative_alloc_count","alloc_size","cumulative_alloc_size","parent_id","source_file","line_number"
          0,"experimental_flamegraph",605908369259172,0,"__start_thread","/apex/com.android.runtime/lib64/bionic/libc.so",0,8,0,84848,0,210,0,1084996,"[NULL]","[NULL]","[NULL]"
          1,"experimental_flamegraph",605908369259172,1,"_ZL15__pthread_startPv","/apex/com.android.runtime/lib64/bionic/libc.so",0,8,0,84848,0,210,0,1084996,0,"[NULL]","[NULL]"
          2,"experimental_flamegraph",605908369259172,2,"_ZN7android14AndroidRuntime15javaThreadShellEPv","/system/lib64/libandroid_runtime.so",0,5,0,27704,0,77,0,348050,1,"[NULL]","[NULL]"
          3,"experimental_flamegraph",605908369259172,3,"_ZN7android6Thread11_threadLoopEPv","/system/lib64/libutils.so",0,5,0,27704,0,77,0,348050,2,"[NULL]","[NULL]"
          4,"experimental_flamegraph",605908369259172,4,"_ZN7android10PoolThread10threadLoopEv","/system/lib64/libbinder.so",0,1,0,4096,0,64,0,279182,3,"[NULL]","[NULL]"
          5,"experimental_flamegraph",605908369259172,5,"_ZN7android14IPCThreadState14joinThreadPoolEb","/system/lib64/libbinder.so",0,1,0,4096,0,64,0,279182,4,"[NULL]","[NULL]"
          6,"experimental_flamegraph",605908369259172,6,"_ZN7android14IPCThreadState20getAndExecuteCommandEv","/system/lib64/libbinder.so",0,1,0,4096,0,64,0,279182,5,"[NULL]","[NULL]"
          7,"experimental_flamegraph",605908369259172,7,"_ZN7android14IPCThreadState14executeCommandEi","/system/lib64/libbinder.so",0,1,0,4096,0,64,0,279182,6,"[NULL]","[NULL]"
          8,"experimental_flamegraph",605908369259172,8,"_ZN7android7BBinder8transactEjRKNS_6ParcelEPS1_j","/system/lib64/libbinder.so",0,1,0,4096,0,64,0,279182,7,"[NULL]","[NULL]"
          9,"experimental_flamegraph",605908369259172,9,"_ZN11JavaBBinder10onTransactEjRKN7android6ParcelEPS1_j","/system/lib64/libandroid_runtime.so",0,0,0,0,0,60,0,262730,8,"[NULL]","[NULL]"
        '''))

  def test_heap_profile_tracker_new_stack(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_tracker_new_stack.textproto'),
        query="""
        SELECT * FROM heap_profile_allocation;
        """,
        out=Csv("""
        "id","type","ts","upid","heap_name","callsite_id","count","size"
        0,"heap_profile_allocation",0,0,"unknown",0,1,1
        1,"heap_profile_allocation",0,0,"unknown",0,-1,-1
        2,"heap_profile_allocation",1,0,"unknown",0,1,1
        3,"heap_profile_allocation",1,0,"unknown",0,-1,-1
        """))

  def test_heap_profile_tracker_twoheaps(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_tracker_twoheaps.textproto'),
        query="""
        SELECT * FROM heap_profile_allocation;
        """,
        out=Csv("""
        "id","type","ts","upid","heap_name","callsite_id","count","size"
        0,"heap_profile_allocation",0,0,"libc.malloc",0,1,1
        1,"heap_profile_allocation",0,0,"libc.malloc",0,-1,-1
        2,"heap_profile_allocation",0,0,"custom",0,1,1
        3,"heap_profile_allocation",0,0,"custom",0,-1,-1
        """))

  def test_heap_graph_flamegraph_focused(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_branching.textproto'),
        query="""
        SELECT
          id,
          depth,
          name,
          count,
          cumulative_count,
          size,
          cumulative_size,
          parent_id
        FROM experimental_flamegraph(
          'graph',
          (SELECT max(graph_sample_ts) FROM heap_graph_object),
          NULL,
          (SELECT max(upid) FROM heap_graph_object),
          NULL,
          'left'
        )
        LIMIT 10;
        """,
        out=Path('heap_graph_flamegraph_focused.out'))

  def test_heap_graph_superclass(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_superclass.textproto'),
        query="""
        SELECT c.id, c.superclass_id, c.name, s.name AS superclass_name, c.location
        FROM heap_graph_class c LEFT JOIN heap_graph_class s ON c.superclass_id = s.id;
        """,
        out=Csv("""
        "id","superclass_id","name","superclass_name","location"
        0,"[NULL]","java.lang.Class<java.lang.Object>","[NULL]","l1"
        1,"[NULL]","java.lang.Class<MySuperClass>","[NULL]","l1"
        2,"[NULL]","java.lang.Class<MyChildClass>","[NULL]","l2"
        3,"[NULL]","java.lang.Object","[NULL]","l1"
        4,3,"MySuperClass","java.lang.Object","l1"
        5,4,"MyChildClass","MySuperClass","l2"
        """))

  def test_heap_graph_native_size(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_native_size.textproto'),
        query="""
        SELECT c.name AS type_name,
               o.native_size
        FROM heap_graph_object o JOIN heap_graph_class c ON o.type_id = c.id
        WHERE o.root_type = "ROOT_JAVA_FRAME";
        """,
        out=Csv("""
        "type_name","native_size"
        "android.graphics.Bitmap",123456
        "android.os.BinderProxy",0
        """))
