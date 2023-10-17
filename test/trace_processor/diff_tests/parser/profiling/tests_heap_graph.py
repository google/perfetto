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
        FROM experimental_flamegraph
        WHERE upid = (SELECT max(upid) FROM heap_graph_object)
          AND profile_type = 'graph'
          AND ts = (SELECT max(graph_sample_ts) FROM heap_graph_object)
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
        FROM experimental_flamegraph
        WHERE upid = (SELECT max(upid) FROM heap_graph_object)
          AND profile_type = 'graph'
          AND ts = (SELECT max(graph_sample_ts) FROM heap_graph_object)
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
        FROM experimental_flamegraph
        WHERE upid = (SELECT max(upid) FROM heap_graph_object)
          AND profile_type = 'graph'
          AND ts = (SELECT max(graph_sample_ts) FROM heap_graph_object)
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
        FROM experimental_flamegraph
        WHERE upid = (SELECT max(upid) FROM heap_graph_object)
          AND profile_type = 'graph'
          AND ts = (SELECT max(graph_sample_ts) FROM heap_graph_object)
        LIMIT 10;
        """,
        out=Path('heap_graph_flamegraph_system-server-heap-graph.out'))

  def test_heap_profile_flamegraph_system_server_native_profile(self):
    return DiffTestBlueprint(
        trace=DataPath('system-server-native-profile'),
        query="""
        SELECT * FROM experimental_flamegraph
        WHERE ts = 605908369259172
          AND upid = 1
          AND profile_type = 'native'
        LIMIT 10;
        """,
        out=Path('heap_profile_flamegraph_system-server-native-profile.out'))

  def test_heap_profile_tracker_new_stack(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_tracker_new_stack.textproto'),
        query="""
        SELECT * FROM heap_profile_allocation;
        """,
        out=Csv("""
        "id","type","ts","upid","heap_name","callsite_id","count","size"
        0,"heap_profile_allocation",0,0,"malloc",0,1,1
        1,"heap_profile_allocation",0,0,"malloc",0,-1,-1
        2,"heap_profile_allocation",1,0,"malloc",0,1,1
        3,"heap_profile_allocation",1,0,"malloc",0,-1,-1
        """))

  def test_heap_profile_tracker_twoheaps(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_tracker_twoheaps.textproto'),
        query="""
        SELECT * FROM heap_profile_allocation;
        """,
        out=Csv("""
        "id","type","ts","upid","heap_name","callsite_id","count","size"
        0,"heap_profile_allocation",0,0,"malloc",0,1,1
        1,"heap_profile_allocation",0,0,"malloc",0,-1,-1
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
        FROM experimental_flamegraph
        WHERE upid = (SELECT max(upid) FROM heap_graph_object)
          AND profile_type = 'graph'
          AND ts = (SELECT max(graph_sample_ts) FROM heap_graph_object)
          AND focus_str = 'left'
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
