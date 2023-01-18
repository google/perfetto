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

from python.generators.diff_tests.testing import Path, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Profiling(DiffTestModule):

  def test_heap_profile_jit(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_jit.textproto'),
        query="""
SELECT name, mapping, rel_pc FROM stack_profile_frame ORDER BY name;
""",
        out=Csv("""
"name","mapping","rel_pc"
"java_frame_1",0,4096
"java_frame_2",0,4096
"""))

  def test_heap_profile_deobfuscate(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_deobfuscate.textproto'),
        query=Path('heap_profile_deobfuscate_test.sql'),
        out=Csv("""
"deobfuscated_name","mapping","rel_pc"
"Bar.function1",0,4096
"""))

  def test_heap_profile_deobfuscate_2(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_deobfuscate_memfd.textproto'),
        query=Path('heap_profile_deobfuscate_test.sql'),
        out=Csv("""
"deobfuscated_name","mapping","rel_pc"
"Bar.function1",0,4096
"""))

  def test_heap_profile_dump_max_legacy(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_dump_max_legacy.textproto'),
        query="""
SELECT * FROM heap_profile_allocation;
""",
        out=Csv("""
"id","type","ts","upid","heap_name","callsite_id","count","size"
0,"heap_profile_allocation",-10,2,"malloc",2,0,1000
1,"heap_profile_allocation",-10,2,"malloc",3,0,90
"""))

  def test_heap_profile_dump_max(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_dump_max.textproto'),
        query="""
SELECT * FROM heap_profile_allocation;
""",
        out=Csv("""
"id","type","ts","upid","heap_name","callsite_id","count","size"
0,"heap_profile_allocation",-10,2,"malloc",2,6,1000
1,"heap_profile_allocation",-10,2,"malloc",3,1,90
"""))

  def test_profiler_smaps(self):
    return DiffTestBlueprint(
        trace=Path('profiler_smaps.textproto'),
        query="""
SELECT id, type, upid, ts, path, size_kb, private_dirty_kb, swap_kb
FROM profiler_smaps;
""",
        out=Csv("""
"id","type","upid","ts","path","size_kb","private_dirty_kb","swap_kb"
0,"profiler_smaps",2,10,"/system/lib64/libc.so",20,4,4
1,"profiler_smaps",2,10,"[anon: libc_malloc]",30,10,10
"""))

  def test_profiler_smaps_metric(self):
    return DiffTestBlueprint(
        trace=Path('profiler_smaps.textproto'),
        query=Metric('profiler_smaps'),
        out=TextProto(r"""
profiler_smaps {
  instance {
    process {
      name: "system_server"
      uid: 1000
    }
    mappings {
      path: "[anon: libc_malloc]"
      size_kb: 30
      private_dirty_kb: 10
      swap_kb: 10
    }
    mappings {
      path: "/system/lib64/libc.so"
      size_kb: 20
      private_dirty_kb: 4
      swap_kb: 4
    }
  }
}
"""))

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
        trace=Path('heap_graph_duplicate.textproto'),
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
        trace=Path('../../data/system-server-heap-graph-new.pftrace'),
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
        trace=Path('../../data/system-server-native-profile'),
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

  def test_heap_graph_flamegraph_matches_objects(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_huge_size.textproto'),
        query="""
SELECT
  obj.upid AS upid,
  obj.graph_sample_ts AS ts,
  SUM(obj.self_size + obj.native_size) AS total_objects_size,
  (
    SELECT SUM(cumulative_size)
    FROM experimental_flamegraph
    WHERE experimental_flamegraph.upid = obj.upid
      AND experimental_flamegraph.ts = obj.graph_sample_ts
      AND profile_type = 'graph'
      AND depth = 0 -- only the roots
  ) AS total_flamegraph_size
FROM
  heap_graph_object AS obj
WHERE
  obj.reachable != 0
GROUP BY obj.upid, obj.graph_sample_ts;
""",
        out=Csv("""
"upid","ts","total_objects_size","total_flamegraph_size"
1,10,3000000036,3000000036
"""))

  def test_heap_graph_flamegraph_3(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_legacy.textproto'),
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

  def test_stack_profile_tracker_empty_callstack(self):
    return DiffTestBlueprint(
        trace=Path('stack_profile_tracker_empty_callstack.textproto'),
        query="""
SELECT count(1) AS count FROM heap_profile_allocation;
""",
        out=Csv("""
"count"
0
"""))

  def test_unsymbolized_frames(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_no_symbols.textproto'),
        query=Metric('unsymbolized_frames'),
        out=TextProto(r"""
unsymbolized_frames {
  frames {
    module: "/liblib.so"
    build_id: "6275696c642d6964"
    address: 4096
    google_lookup_id: "6275696c642d6964"
  }
  frames {
    module: "/liblib.so"
    build_id: "6275696c642d6964"
    address: 8192
    google_lookup_id: "6275696c642d6964"
  }
  frames {
    module: "/libmonochrome_64.so"
    build_id: "7f0715c286f8b16c10e4ad349cda3b9b56c7a773"
    address: 4096
    google_lookup_id: "c215077ff8866cb110e4ad349cda3b9b0"
  }
  frames {
    module: "/libmonochrome_64.so"
    build_id: "7f0715c286f8b16c10e4ad349cda3b9b56c7a773"
    address: 8192
    google_lookup_id: "c215077ff8866cb110e4ad349cda3b9b0"
  }
}
"""))

  def test_simpleperf_event(self):
    return DiffTestBlueprint(
        trace=Path('simpleperf_event.py'),
        query=Metric('android_simpleperf'),
        out=Path('simpleperf_event.out'))

  def test_java_heap_stats(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph.textproto'),
        query=Metric('java_heap_stats'),
        out=TextProto(r"""
java_heap_stats {
  instance_stats {
    upid: 2
    process {
      name: "system_server"
      uid: 1000
    }
    samples {
      ts: 10
      heap_size: 1760
      heap_native_size: 0
      reachable_heap_size: 352
      reachable_heap_native_size: 0
      obj_count: 6
      reachable_obj_count: 3
      anon_rss_and_swap_size: 4096000
      roots {
        root_type: "ROOT_JAVA_FRAME"
        type_name: "DeobfuscatedA[]"
        obj_count: 1
      }
      roots {
        root_type: "ROOT_JAVA_FRAME"
        type_name: "FactoryProducerDelegateImplActor"
        obj_count: 1
      }
    }
  }
}
"""))

  def test_heap_stats_closest_proc(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_closest_proc.textproto'),
        query=Metric('java_heap_stats'),
        out=Path('heap_stats_closest_proc.out'))

  def test_java_heap_histogram(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph.textproto'),
        query=Metric('java_heap_histogram'),
        out=Path('java_heap_histogram.out'))

  def test_perf_sample_rvc(self):
    return DiffTestBlueprint(
        trace=Path('../../data/perf_sample.pb'),
        query="""
SELECT ps.ts, ps.cpu, ps.cpu_mode, ps.unwind_error, ps.perf_session_id,
       pct.name AS cntr_name, pct.is_timebase,
       thread.tid,
       spf.name
FROM experimental_annotated_callstack eac
JOIN perf_sample ps
  ON (eac.start_id = ps.callsite_id)
JOIN perf_counter_track pct
  USING(perf_session_id, cpu)
JOIN thread
  USING(utid)
JOIN stack_profile_frame spf
  ON (eac.frame_id = spf.id)
ORDER BY ps.ts ASC, eac.depth ASC;
""",
        out=Path('perf_sample_rvc.out'))

  def test_perf_sample_sc(self):
    return DiffTestBlueprint(
        trace=Path('../../data/perf_sample_sc.pb'),
        query="""
SELECT ps.ts, ps.cpu, ps.cpu_mode, ps.unwind_error, ps.perf_session_id,
       pct.name AS cntr_name, pct.is_timebase,
       thread.tid,
       spf.name
FROM experimental_annotated_callstack eac
JOIN perf_sample ps
  ON (eac.start_id = ps.callsite_id)
JOIN perf_counter_track pct
  USING(perf_session_id, cpu)
JOIN thread
  USING(utid)
JOIN stack_profile_frame spf
  ON (eac.frame_id = spf.id)
ORDER BY ps.ts ASC, eac.depth ASC;
""",
        out=Path('perf_sample_sc.out'))

  def test_stack_profile_symbols(self):
    return DiffTestBlueprint(
        trace=Path('../../data/heapprofd_standalone_client_example-trace'),
        query="""
SELECT name, source_file, line_number FROM stack_profile_symbol;
""",
        out=Path('stack_profile_symbols.out'))

  def test_callstack_sampling_flamegraph(self):
    return DiffTestBlueprint(
        trace=Path('../../data/callstack_sampling.pftrace'),
        query="""
SELECT ef.*
FROM experimental_flamegraph ef
JOIN process USING (upid)
WHERE pid = 1728
  AND profile_type = 'perf'
  AND ts <= 7689491063351
LIMIT 10;
""",
        out=Path('callstack_sampling_flamegraph.out'))

  def test_callstack_sampling_flamegraph_multi_process(self):
    return DiffTestBlueprint(
        trace=Path('../../data/callstack_sampling.pftrace'),
        query="""
SELECT count(*) AS count, 'BothProcesses' AS description
FROM experimental_flamegraph
WHERE
  upid_group = (
    SELECT group_concat(DISTINCT upid)
    FROM perf_sample JOIN thread t USING (utid) JOIN process p USING (upid)
  )
  AND profile_type = 'perf'
  AND ts <= 7689491063351
  AND size > 0
UNION ALL
SELECT count(*) AS count, 'FirstProcess' AS description
FROM experimental_flamegraph
JOIN process USING (upid)
WHERE pid = 1728
  AND profile_type = 'perf'
  AND ts <= 7689491063351
  AND size > 0
UNION ALL
SELECT count(*) AS count, 'SecondProcess' AS description
FROM experimental_flamegraph
JOIN process USING (upid)
WHERE pid = 703
  AND profile_type = 'perf'
  AND ts <= 7689491063351
  AND size > 0;
""",
        out=Csv("""
"count","description"
658,"BothProcesses"
483,"FirstProcess"
175,"SecondProcess"
"""))

  def test_no_build_id(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_data_local_tmp.textproto'),
        query="""
SELECT value FROM stats WHERE name = 'symbolization_tmp_build_id_not_found';
""",
        out=Csv("""
"value"
1
"""))
