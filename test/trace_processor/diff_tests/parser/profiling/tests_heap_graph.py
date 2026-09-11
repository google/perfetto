#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

  def test_heap_graph_bitmap_fields(self):
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
            types {
              id: 1
              class_name: "android.graphics.Bitmap"
            }
            objects {
              id: 1
              type_id: 1
              self_size: 64
              bitmap_id_field: 123
              bitmap_source_id_field: 456
              bitmap_width_field: 100
              bitmap_height_field: 200
            }
            continued: false
            index: 0
          }
        }
        """),
        query="""
        SELECT
          p.field_name,
          p.field_type,
          p.long_value,
          p.int_value
        FROM heap_graph_object o
          JOIN heap_graph_object_data d ON o.object_data_id = d.id
          JOIN heap_graph_primitive p USING (field_set_id)
        ORDER BY p.field_name;
        """,
        out=Csv('''
          "field_name","field_type","long_value","int_value"
          "android.graphics.Bitmap.mHeight","int","[NULL]",200
          "android.graphics.Bitmap.mId","long",123,"[NULL]"
          "android.graphics.Bitmap.mSourceId","long",456,"[NULL]"
          "android.graphics.Bitmap.mWidth","int","[NULL]",100
        '''))

  def test_heap_graph_object(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_baseapk.textproto'),
        query="""
        SELECT o.id,
               o.upid,
               o.graph_sample_ts,
               o.self_size,
               o.reference_set_id,
               o.reachable,
               o.heap_type,
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
          SELECT
            id,
            reference_set_id,
            owner_id,
            owned_id,
            field_name,
            field_type_name,
            deobfuscated_field_name
          FROM heap_graph_reference;
        """,
        out=Path('heap_graph_reference.out'))

  def test_heap_graph_object_2(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_deobfuscate_pkg.textproto'),
        query="""
        SELECT o.id,
               o.upid,
               o.graph_sample_ts,
               o.self_size,
               o.reference_set_id,
               o.reachable,
               o.heap_type,
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
          [com.android.art.tracing.ArtHeapGraphTracePacket.heap_graph] {
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
        out=Csv('''
          "id","depth","name","map_name","count","cumulative_count","size","cumulative_size","parent_id"
          0,0,"FactoryProducerDelegateImplActor [ROOT_JAVA_FRAME]","JAVA",1,2,64,96,"[NULL]"
          1,1,"Foo","JAVA",1,1,32,32,0
          2,0,"DeobfuscatedA[] [ROOT_JAVA_FRAME]","JAVA",1,1,256,256,"[NULL]"
          3,0,"android.os.Parcel [ROOT_VM_INTERNAL]","JAVA",1,1,256,256,"[NULL]"
                '''))

  def test_heap_graph_flamegraph_truncated(self):
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
            continued: false
            # Starting after packet zero marks the graph as truncated.
            index: 1
          }
        }
        """),
        query="""
        SELECT
          depth,
          name,
          map_name,
          count,
          cumulative_count,
          size,
          cumulative_size
        FROM experimental_flamegraph(
          'graph',
          (SELECT ts FROM heap_graph),
          NULL,
          (SELECT upid FROM heap_graph),
          NULL,
          NULL
        );
        """,
        out=Csv('''
          "depth","name","map_name","count","cumulative_count","size","cumulative_size"
          0,"ERROR: INCOMPLETE GRAPH (try increasing buffer size)","JAVA",1,1,1,1
        '''))

  def test_heap_graph_object_3(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph.textproto'),
        query="""
        SELECT o.id,
               o.upid,
               o.graph_sample_ts,
               o.self_size,
               o.reference_set_id,
               o.reachable,
               o.heap_type,
               c.name AS type_name,
               c.deobfuscated_name AS deobfuscated_type_name,
               o.root_type
        FROM heap_graph_object o JOIN heap_graph_class c ON o.type_id = c.id;
        """,
        out=Csv('''
          "id","upid","graph_sample_ts","self_size","reference_set_id","reachable","heap_type","type_name","deobfuscated_type_name","root_type"
          0,2,10,64,0,1,"HEAP_TYPE_APP","FactoryProducerDelegateImplActor","[NULL]","ROOT_JAVA_FRAME"
          1,2,10,32,"[NULL]",1,"HEAP_TYPE_APP","Foo","[NULL]","[NULL]"
          2,2,10,128,"[NULL]",0,"HEAP_TYPE_APP","Foo","[NULL]","[NULL]"
          3,2,10,1024,3,0,"HEAP_TYPE_APP","a","DeobfuscatedA","[NULL]"
          4,2,10,256,"[NULL]",1,"HEAP_TYPE_APP","a[]","DeobfuscatedA[]","ROOT_JAVA_FRAME"
          5,2,10,256,"[NULL]",0,"HEAP_TYPE_APP","java.lang.Class<a[]>","java.lang.Class<DeobfuscatedA[]>","[NULL]"
          6,2,10,256,"[NULL]",1,"HEAP_TYPE_ZYGOTE","android.os.Parcel","[NULL]","ROOT_VM_INTERNAL"
        '''))

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
          SELECT
            id,
            reference_set_id,
            owner_id,
            owned_id,
            field_name,
            field_type_name,
            deobfuscated_field_name
          FROM heap_graph_reference;
        """,
        out=Path('heap_graph_reference.out'))

  def test_heap_graph_two_locations(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_two_locations.textproto'),
        query="""
        SELECT o.id,
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
        out=Csv('''
          "id","upid","graph_sample_ts","self_size","reference_set_id","reachable","type_name","deobfuscated_type_name","root_type"
          0,2,10,64,0,1,"FactoryProducerDelegateImplActor","[NULL]","ROOT_JAVA_FRAME"
          1,2,10,32,"[NULL]",1,"a","Foo","[NULL]"
          2,2,10,128,"[NULL]",0,"a","Foo","[NULL]"
          3,2,10,256,2,0,"a","DeobfuscatedA","[NULL]"
          4,2,10,256,"[NULL]",1,"a[]","DeobfuscatedA[]","ROOT_JAVA_FRAME"
          5,2,10,256,"[NULL]",0,"java.lang.Class<a[]>","java.lang.Class<DeobfuscatedA[]>","[NULL]"
        '''))

  def test_heap_graph_object_4(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_legacy.textproto'),
        query="""
        SELECT o.id,
               o.upid,
               o.graph_sample_ts,
               o.self_size,
               o.reference_set_id,
               o.reachable,
               o.heap_type,
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
          SELECT
              id,
              reference_set_id,
              owner_id,
              owned_id,
              field_name,
              field_type_name,
              deobfuscated_field_name
          FROM heap_graph_reference;
        """,
        out=Path('heap_graph_reference.out'))

  def test_heap_graph_interleaved_object(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_interleaved.textproto'),
        query="""
        SELECT o.id,
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
        out=Csv('''
          "id","upid","graph_sample_ts","self_size","reference_set_id","reachable","type_name","deobfuscated_type_name","root_type"
          0,2,10,64,0,1,"FactoryProducerDelegateImplActor","[NULL]","ROOT_JAVA_FRAME"
          1,2,10,32,"[NULL]",1,"Foo","[NULL]","[NULL]"
          2,2,10,128,"[NULL]",0,"Foo","[NULL]","[NULL]"
          3,2,10,256,1,0,"a","DeobfuscatedA","[NULL]"
          4,3,10,64,"[NULL]",1,"FactoryProducerDelegateImplActor","[NULL]","ROOT_JAVA_FRAME"
        '''))

  def test_heap_graph_interleaved_reference(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_interleaved.textproto'),
        query="""
          SELECT
            id,
            reference_set_id,
            owner_id,
            owned_id,
            field_name,
            field_type_name,
            deobfuscated_field_name
          FROM heap_graph_reference;
        """,
        out=Csv('''
          "id","reference_set_id","owner_id","owned_id","field_name","field_type_name","deobfuscated_field_name"
          0,0,0,1,"FactoryProducerDelegateImplActor.foo","[NULL]","[NULL]"
          1,1,3,0,"a.a","[NULL]","DeobfuscatedA.deobfuscatedA"
        '''))

  def test_heap_graph_flamegraph_system_server_heap_graph(self):
    return DiffTestBlueprint(
        trace=DataPath('system-server-heap-graph-new.pftrace'),
        query="""
        SELECT
          depth,
          name,
          map_name,
          count,
          cumulative_count,
          size,
          cumulative_size
        FROM experimental_flamegraph(
          'graph',
          (SELECT max(graph_sample_ts) FROM heap_graph_object),
          NULL,
          (SELECT max(upid) FROM heap_graph_object),
          NULL,
          NULL
        )
        ORDER BY cumulative_size DESC, name
        LIMIT 10;
        """,
        out=Path('heap_graph_flamegraph_system-server-heap-graph.out'))

  def test_heap_graph_root_sorting(self):
    # We expect RootA to be the parent because it comes first alphabetically
    return DiffTestBlueprint(
        trace=Path('heap_graph_root_sorting.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.memory.heap_graph.class_tree;
        SELECT
          p.name AS parent_of_child
        FROM _heap_graph_class_tree c
        JOIN _heap_graph_class_tree p ON c.parent_id = p.id
        WHERE c.name = 'Child';
        """,
        out=Csv('''
          "parent_of_child"
          "RootA"
        '''))

  def test_heap_graph_root_sorting_reverse(self):
    # We expect RootA to be the parent because it comes first alphabetically
    return DiffTestBlueprint(
        trace=Path('heap_graph_root_sorting_reverse.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.memory.heap_graph.class_tree;
        SELECT
          p.name AS parent_of_child
        FROM _heap_graph_class_tree c
        JOIN _heap_graph_class_tree p ON c.parent_id = p.id
        WHERE c.name = 'Child';
        """,
        out=Csv('''
          "parent_of_child"
          "RootA"
        '''))

  def test_heap_profile_flamegraph_system_server_native_profile(self):
    return DiffTestBlueprint(
        trace=DataPath('system-server-native-profile'),
        query="""
        SELECT
          ts,
          depth,
          name,
          map_name,
          count,
          cumulative_count,
          size,
          cumulative_size,
          alloc_count,
          cumulative_alloc_count,
          alloc_size,
          cumulative_alloc_size,
          source_file,
          line_number
        FROM experimental_flamegraph(
          'native',
          605908369259172,
          NULL,
          1,
          NULL,
          NULL
        )
        ORDER BY depth, name, map_name, cumulative_size, cumulative_count
        LIMIT 10;
        """,
        out=Csv('''
          "ts","depth","name","map_name","count","cumulative_count","size","cumulative_size","alloc_count","cumulative_alloc_count","alloc_size","cumulative_alloc_size","source_file","line_number"
          605908369259172,0,"__libc_init","/apex/com.android.runtime/lib64/bionic/libc.so",0,0,0,0,0,7,0,29012,"[NULL]","[NULL]"
          605908369259172,0,"__start_thread","/apex/com.android.runtime/lib64/bionic/libc.so",0,8,0,84848,0,210,0,1084996,"[NULL]","[NULL]"
          605908369259172,1,"_ZL15__pthread_startPv","/apex/com.android.runtime/lib64/bionic/libc.so",0,8,0,84848,0,210,0,1084996,"[NULL]","[NULL]"
          605908369259172,1,"main","/system/bin/app_process64",0,0,0,0,0,7,0,29012,"[NULL]","[NULL]"
          605908369259172,2,"_ZN3art6Thread14CreateCallbackEPv","/apex/com.android.art/lib64/libart.so",0,3,0,57144,0,133,0,736946,"[NULL]","[NULL]"
          605908369259172,2,"_ZN7android14AndroidRuntime15javaThreadShellEPv","/system/lib64/libandroid_runtime.so",0,5,0,27704,0,77,0,348050,"[NULL]","[NULL]"
          605908369259172,2,"_ZN7android14AndroidRuntime5startEPKcRKNS_6VectorINS_7String8EEEb","/system/lib64/libandroid_runtime.so",0,0,0,0,0,7,0,29012,"[NULL]","[NULL]"
          605908369259172,3,"_ZN3art35InvokeVirtualOrInterfaceWithJValuesIPNS_9ArtMethodEEENS_6JValueERKNS_33ScopedObjectAccessAlreadyRunnableEP8_jobjectT_PK6jvalue","/apex/com.android.art/lib64/libart.so",0,3,0,57144,0,133,0,736946,"[NULL]","[NULL]"
          605908369259172,3,"_ZN7_JNIEnv20CallStaticVoidMethodEP7_jclassP10_jmethodIDz","/system/lib64/libandroid_runtime.so",0,0,0,0,0,7,0,29012,"[NULL]","[NULL]"
          605908369259172,3,"_ZN7android6Thread11_threadLoopEPv","/system/lib64/libutils.so",0,5,0,27704,0,77,0,348050,"[NULL]","[NULL]"
        '''))

  def test_heap_profile_tracker_new_stack(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_tracker_new_stack.textproto'),
        query="""
        SELECT id, ts, upid, heap_name, callsite_id, count, size
        FROM heap_profile_allocation;
        """,
        out=Csv("""
        "id","ts","upid","heap_name","callsite_id","count","size"
        0,0,0,"unknown",0,1,1
        1,0,0,"unknown",0,-1,-1
        2,1,0,"unknown",0,1,1
        3,1,0,"unknown",0,-1,-1
        """))

  def test_heap_profile_tracker_twoheaps(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_tracker_twoheaps.textproto'),
        query="""
        SELECT id, ts, upid, heap_name, callsite_id, count, size
        FROM heap_profile_allocation;
        """,
        out=Csv("""
        "id","ts","upid","heap_name","callsite_id","count","size"
        0,0,0,"libc.malloc",0,1,1
        1,0,0,"libc.malloc",0,-1,-1
        2,0,0,"custom",0,1,1
        3,0,0,"custom",0,-1,-1
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

  def test_heap_graph_native_size_flamegraph(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_native_size.textproto'),
        query="""
        SELECT
          depth,
          name,
          map_name,
          count,
          cumulative_count,
          size,
          cumulative_size,
          alloc_count,
          cumulative_alloc_count,
          alloc_size,
          cumulative_alloc_size
        FROM experimental_flamegraph(
          'graph',
          (SELECT max(graph_sample_ts) FROM heap_graph_object),
          NULL,
          (SELECT max(upid) FROM heap_graph_object),
          NULL,
          NULL
        )
        WHERE name LIKE '[native] %';
        """,
        out=Csv('''
          "depth","name","map_name","count","cumulative_count","size","cumulative_size","alloc_count","cumulative_alloc_count","alloc_size","cumulative_alloc_size"
          1,"[native] android.graphics.Bitmap [ROOT_JAVA_FRAME]","JAVA",1,1,123456,123456,0,0,0,0
        '''))

  def test_heap_graph_runtime_internal_(self):
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
            types {
              id: 3
              class_name: "java.lang.Object"
            }
            types {
              id: 4
              class_name: "java.lang.DexCache"
              superclass_id: 3
            }
            roots {
              root_type: ROOT_JAVA_FRAME
              object_ids: 5
            }
            objects {
              id: 5
              type_id: 4
              runtime_internal_object_id: 6
              self_size: 64
            }
            objects {
              id: 6
              type_id: 3
              self_size: 32
            }
            continued: false
            index: 0
          }
        }
        """),
        query="""
        SELECT
          r.field_name,
          ca.name owner,
          cb.name owned
        FROM heap_graph_reference r
        JOIN heap_graph_object a ON a.id = r.owner_id
        JOIN heap_graph_class ca ON ca.id = a.type_id
        JOIN heap_graph_object b ON b.id = r.owned_id
        JOIN heap_graph_class cb ON cb.id = b.type_id
        """,
        out=Csv("""
        "field_name","owner","owned"
        "runtimeInternalObjects","java.lang.DexCache","java.lang.Object"
        """))

  def test_heap_graph_heap_size(self):
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
            heap_bytes_allocated: 100000
            types {
              id: 1
              class_name: "java.lang.Object"
            }
            objects {
              id: 1
              type_id: 1
              self_size: 64
            }
            continued: false
            index: 0
          }
        }
        """),
        query="""
        SELECT ts, heap_size
        FROM heap_graph;
        """,
        out=Csv("""
        "ts","heap_size"
        10,100000
        """))

  def test_heap_graph_merged_classes(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          process_tree {
            processes {
              pid: 2
              ppid: 1
              cmdline: "com.example.app"
              uid: 10001
            }
          }
        }
        packet {
          packages_list {
            packages {
              name: "com.example.app"
              uid: 10001
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 999
          timestamp: 10
          [com.android.art.tracing.ArtHeapGraphTracePacket.heap_graph] {
            pid: 2
            types {
              id: 1
              class_name: "MergedBase"
            }
            # Object 0: Matches discriminator_id: 0 ($cid = 0) -> the base class itself -> MergedBase
            objects {
              id: 0
              type_id: 1
              self_size: 64
              merged_class_discriminators {
                field_name: "$cid"
                value: 0
              }
            }
            # Object 1: Matches nested branch $cid = 1 and nested leaf $cid2 = 10 -> NestedLeaf
            objects {
              id: 1
              type_id: 1
              self_size: 64
              merged_class_discriminators {
                field_name: "$cid"
                value: 1
              }
              merged_class_discriminators {
                field_name: "$cid2"
                value: 10
              }
            }
            # Object 2: Matches direct leaf discriminator_id: 2 ($cid = 2) -> DirectLeaf2
            objects {
              id: 2
              type_id: 1
              self_size: 64
              merged_class_discriminators {
                field_name: "$cid"
                value: 2
              }
            }
            # Object 3: Duplicate class name matching base class ($cid = 3) -> MergedBase
            objects {
              id: 3
              type_id: 1
              self_size: 64
              merged_class_discriminators {
                field_name: "$cid"
                value: 3
              }
            }
            # Object 4: Unknown discriminator value ($cid = 999) -> Ambiguous -> MergedBase
            objects {
              id: 4
              type_id: 1
              self_size: 64
              merged_class_discriminators {
                field_name: "$cid"
                value: 999
              }
            }
            # Object 5: No discriminators -> Base class row -> MergedBase
            objects {
              id: 5
              type_id: 1
              self_size: 64
            }
            # Object 6: Matches nested branch $cid = 1 with no nested $cid2 -> NestedBase
            objects {
              id: 6
              type_id: 1
              self_size: 64
              merged_class_discriminators {
                field_name: "$cid"
                value: 1
              }
            }
            # Object 7: Matches nested branch $cid = 1 with nested $cid2 = 0 -> NestedBase
            objects {
              id: 7
              type_id: 1
              self_size: 64
              merged_class_discriminators {
                field_name: "$cid"
                value: 1
              }
              merged_class_discriminators {
                field_name: "$cid2"
                value: 0
              }
            }
            # Object 8: Matches nested branch $cid = 1 with unknown $cid2 = 99 -> Ambiguous -> MergedBase
            objects {
              id: 8
              type_id: 1
              self_size: 64
              merged_class_discriminators {
                field_name: "$cid"
                value: 1
              }
              merged_class_discriminators {
                field_name: "$cid2"
                value: 99
              }
            }
            # Object 9: Matches discriminator_id: 4 ($cid = 4) having single mismatched child -> Ambiguous -> MergedBase
            objects {
              id: 9
              type_id: 1
              self_size: 64
              merged_class_discriminators {
                field_name: "$cid"
                value: 4
              }
            }
            continued: false
            index: 0
          }
        }
        packet {
          deobfuscation_mapping {
            package_name: "com.example.app"
            obfuscated_classes {
              obfuscated_name: "MergedBase"
              deobfuscated_name: "com.example.MergedBase"
              merged_classes {
                class_id_field_name: "$cid"
                # First child is the class itself
                merged_classes {
                  class_id: 0
                  name: "com.example.MergedBase"
                }
                # Merged class with its own name and multiple merged classes
                # First child is the class itself (NestedBase)
                merged_classes {
                  class_id: 1
                  name: "com.example.NestedBase"
                  merged_classes {
                    class_id_field_name: "$cid2"
                    merged_classes {
                      class_id: 0
                      name: "com.example.NestedBase"
                    }
                    merged_classes {
                      class_id: 10
                      name: "com.example.NestedLeaf"
                    }
                  }
                }
                # Direct leaf
                merged_classes {
                  class_id: 2
                  name: "com.example.DirectLeaf2"
                }
                # Corner case: duplicate name matching base class
                merged_classes {
                  class_id: 3
                  name: "com.example.MergedBase"
                }
                # Corner case: single child with a mismatched name (ambiguous)
                merged_classes {
                  class_id: 4
                  name: "com.example.MismatchOwner"
                  merged_classes {
                    merged_classes {
                      name: "com.example.MismatchChild"
                    }
                  }
                }
              }
            }
          }
        }
        """),
        query="""
        SELECT
          o.id AS object_id,
          c.name AS obfuscated_name,
          c.deobfuscated_name,
          -- Verify whether this object shares type_id with the base class row (object 5)
          o.type_id = (SELECT type_id FROM heap_graph_object WHERE id = 5) AS shares_base_type_id,
          -- Verify total number of heap_graph_class rows created
          (SELECT count() FROM heap_graph_class) AS total_classes
        FROM heap_graph_object o
        JOIN heap_graph_class c ON o.type_id = c.id
        ORDER BY o.id;
        """,
        out=Csv("""
        "object_id","obfuscated_name","deobfuscated_name","shares_base_type_id","total_classes"
        0,"MergedBase","com.example.MergedBase",1,4
        1,"MergedBase","com.example.NestedLeaf",0,4
        2,"MergedBase","com.example.DirectLeaf2",0,4
        3,"MergedBase","com.example.MergedBase",1,4
        4,"MergedBase","com.example.MergedBase",1,4
        5,"MergedBase","com.example.MergedBase",1,4
        6,"MergedBase","com.example.NestedBase",0,4
        7,"MergedBase","com.example.NestedBase",0,4
        8,"MergedBase","com.example.MergedBase",1,4
        9,"MergedBase","com.example.MergedBase",1,4
        """))

  def test_heap_graph_merged_classes_missing_field_name(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          process_tree {
            processes {
              pid: 2
              ppid: 1
              cmdline: "com.example.app"
              uid: 10001
            }
          }
        }
        packet {
          packages_list {
            packages {
              name: "com.example.app"
              uid: 10001
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 999
          timestamp: 10
          [com.android.art.tracing.ArtHeapGraphTracePacket.heap_graph] {
            pid: 2
            types {
              id: 1
              class_name: "MergedRoot"
            }
            # Object 0: Matches Child A's nested discriminator $cidA = 1 -> GroupALeaf1
            objects {
              id: 0
              type_id: 1
              self_size: 64
              merged_class_discriminators {
                field_name: "$cidA"
                value: 1
              }
            }
            # Object 1: Matches Child A's nested discriminator $cidA = 2 -> GroupALeaf2
            objects {
              id: 1
              type_id: 1
              self_size: 64
              merged_class_discriminators {
                field_name: "$cidA"
                value: 2
              }
            }
            # Object 2: Matches Child B's nested discriminator $cidB = 10 -> GroupBLeaf
            objects {
              id: 2
              type_id: 1
              self_size: 64
              merged_class_discriminators {
                field_name: "$cidB"
                value: 10
              }
            }
            # Object 3: 0 non-zero discriminators -> Resolves to MergedRoot
            objects {
              id: 3
              type_id: 1
              self_size: 64
              merged_class_discriminators {
                field_name: "$cidA"
                value: 0
              }
            }
            # Object 4: No discriminators -> MergedRoot
            objects {
              id: 4
              type_id: 1
              self_size: 64
            }
            # Object 5: Unknown discriminator value -> Ambiguous -> MergedRoot
            objects {
              id: 5
              type_id: 1
              self_size: 64
              merged_class_discriminators {
                field_name: "$cidA"
                value: 999
              }
            }
            # Object 6: Conflicting discriminators from both branches -> Ambiguous -> MergedRoot
            objects {
              id: 6
              type_id: 1
              self_size: 64
              merged_class_discriminators {
                field_name: "$cidA"
                value: 1
              }
              merged_class_discriminators {
                field_name: "$cidB"
                value: 10
              }
            }
            continued: false
            index: 0
          }
        }
        packet {
          deobfuscation_mapping {
            package_name: "com.example.app"
            obfuscated_classes {
              obfuscated_name: "MergedRoot"
              deobfuscated_name: "com.example.MergedRoot"
              merged_classes {
                # Notice: NO class_id_field_name at this top level!
                # In practice, each merged class has a name, and if it has multiple merged classes,
                # typically its first child is the class itself (with its name).
                merged_classes {
                  name: "com.example.MergedRoot"
                }
                merged_classes {
                  name: "com.example.GroupA"
                  merged_classes {
                    class_id_field_name: "$cidA"
                    # First child is the class itself
                    merged_classes {
                      class_id: 0
                      name: "com.example.GroupA"
                    }
                    merged_classes {
                      class_id: 1
                      name: "com.example.GroupALeaf1"
                    }
                    merged_classes {
                      class_id: 2
                      name: "com.example.GroupALeaf2"
                    }
                  }
                }
                merged_classes {
                  name: "com.example.GroupB"
                  merged_classes {
                    class_id_field_name: "$cidB"
                    # First child is the class itself
                    merged_classes {
                      class_id: 0
                      name: "com.example.GroupB"
                    }
                    merged_classes {
                      class_id: 10
                      name: "com.example.GroupBLeaf"
                    }
                  }
                }
              }
            }
          }
        }
        """),
        query="""
        SELECT
          o.id AS object_id,
          c.name AS obfuscated_name,
          c.deobfuscated_name,
          -- Verify whether this object shares type_id with the base class row (object 4)
          o.type_id = (SELECT type_id FROM heap_graph_object WHERE id = 4) AS shares_base_type_id,
          -- Verify total number of heap_graph_class rows created
          (SELECT count() FROM heap_graph_class) AS total_classes
        FROM heap_graph_object o
        JOIN heap_graph_class c ON o.type_id = c.id
        ORDER BY o.id;
        """,
        out=Csv("""
        "object_id","obfuscated_name","deobfuscated_name","shares_base_type_id","total_classes"
        0,"MergedRoot","com.example.GroupALeaf1",0,4
        1,"MergedRoot","com.example.GroupALeaf2",0,4
        2,"MergedRoot","com.example.GroupBLeaf",0,4
        3,"MergedRoot","com.example.MergedRoot",1,4
        4,"MergedRoot","com.example.MergedRoot",1,4
        5,"MergedRoot","com.example.MergedRoot",1,4
        6,"MergedRoot","com.example.MergedRoot",1,4
        """))
