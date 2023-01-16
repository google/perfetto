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
        query=Path('heap_profile_frames_test.sql'),
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
        query=Path('heap_profile_tracker_new_stack_test.sql'),
        out=Csv("""
"id","type","ts","upid","heap_name","callsite_id","count","size"
0,"heap_profile_allocation",-10,2,"malloc",2,0,1000
1,"heap_profile_allocation",-10,2,"malloc",3,0,90
"""))

  def test_heap_profile_dump_max(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_dump_max.textproto'),
        query=Path('heap_profile_tracker_new_stack_test.sql'),
        out=Csv("""
"id","type","ts","upid","heap_name","callsite_id","count","size"
0,"heap_profile_allocation",-10,2,"malloc",2,6,1000
1,"heap_profile_allocation",-10,2,"malloc",3,1,90
"""))

  def test_profiler_smaps(self):
    return DiffTestBlueprint(
        trace=Path('profiler_smaps.textproto'),
        query=Path('profiler_smaps_test.sql'),
        out=Csv("""
"id","type","upid","ts","path","size_kb","private_dirty_kb","swap_kb"
0,"profiler_smaps",2,10,"/system/lib64/libc.so",20,4,4
1,"profiler_smaps",2,10,"[anon: libc_malloc]",30,10,10
"""))

  def test_profiler_smaps_metric(self):
    return DiffTestBlueprint(
        trace=Path('profiler_smaps.textproto'),
        query=Metric('profiler_smaps'),
        out=Path('profiler_smaps_metric.out'))

  def test_heap_graph_flamegraph(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_baseapk.textproto'),
        query=Path('heap_graph_flamegraph_test.sql'),
        out=Path('heap_graph_flamegraph.out'))

  def test_heap_graph_object(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_baseapk.textproto'),
        query=Path('heap_graph_object_test.sql'),
        out=Path('heap_graph_object.out'))

  def test_heap_graph_reference(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_baseapk.textproto'),
        query=Path('heap_graph_reference_test.sql'),
        out=Path('heap_graph_reference.out'))

  def test_heap_graph_object_2(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_deobfuscate_pkg.textproto'),
        query=Path('heap_graph_object_test.sql'),
        out=Path('heap_graph_object.out'))

  def test_heap_graph_duplicate_flamegraph(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_duplicate.textproto'),
        query=Path('heap_graph_flamegraph_test.sql'),
        out=Path('heap_graph_duplicate_flamegraph.out'))

  def test_heap_graph_flamegraph_2(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph.textproto'),
        query=Path('heap_graph_flamegraph_test.sql'),
        out=Path('heap_graph_flamegraph.out'))

  def test_heap_graph_object_3(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph.textproto'),
        query=Path('heap_graph_object_test.sql'),
        out=Path('heap_graph_object.out'))

  def test_heap_graph_reference_2(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph.textproto'),
        query=Path('heap_graph_reference_test.sql'),
        out=Path('heap_graph_reference.out'))

  def test_heap_graph_two_locations(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_two_locations.textproto'),
        query=Path('heap_graph_object_test.sql'),
        out=Path('heap_graph_two_locations.out'))

  def test_heap_graph_object_4(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_legacy.textproto'),
        query=Path('heap_graph_object_test.sql'),
        out=Path('heap_graph_object.out'))

  def test_heap_graph_reference_3(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_legacy.textproto'),
        query=Path('heap_graph_reference_test.sql'),
        out=Path('heap_graph_reference.out'))

  def test_heap_graph_interleaved_object(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_interleaved.textproto'),
        query=Path('heap_graph_object_test.sql'),
        out=Path('heap_graph_interleaved_object.out'))

  def test_heap_graph_interleaved_reference(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_interleaved.textproto'),
        query=Path('heap_graph_reference_test.sql'),
        out=Path('heap_graph_interleaved_reference.out'))

  def test_heap_graph_flamegraph_system_server_heap_graph(self):
    return DiffTestBlueprint(
        trace=Path('../../data/system-server-heap-graph-new.pftrace'),
        query=Path('heap_graph_flamegraph_test.sql'),
        out=Path('heap_graph_flamegraph_system-server-heap-graph.out'))

  def test_heap_profile_flamegraph_system_server_native_profile(self):
    return DiffTestBlueprint(
        trace=Path('../../data/system-server-native-profile'),
        query=Path('heap_profile_flamegraph_test.sql'),
        out=Path('heap_profile_flamegraph_system-server-native-profile.out'))

  def test_heap_profile_tracker_new_stack(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_tracker_new_stack.textproto'),
        query=Path('heap_profile_tracker_new_stack_test.sql'),
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
        query=Path('heap_profile_tracker_twoheaps_test.sql'),
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
        query=Path('heap_graph_flamegraph_focused_test.sql'),
        out=Path('heap_graph_flamegraph_focused.out'))

  def test_heap_graph_superclass(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_superclass.textproto'),
        query=Path('heap_graph_superclass_test.sql'),
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
        query=Path('heap_graph_native_size_test.sql'),
        out=Csv("""
"type_name","native_size"
"android.graphics.Bitmap",123456
"android.os.BinderProxy",0
"""))

  def test_heap_graph_flamegraph_matches_objects(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_huge_size.textproto'),
        query=Path('heap_graph_flamegraph_matches_objects_test.sql'),
        out=Csv("""
"upid","ts","total_objects_size","total_flamegraph_size"
1,10,3000000036,3000000036
"""))

  def test_heap_graph_flamegraph_3(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_legacy.textproto'),
        query=Path('heap_graph_flamegraph_test.sql'),
        out=Path('heap_graph_flamegraph.out'))

  def test_stack_profile_tracker_empty_callstack(self):
    return DiffTestBlueprint(
        trace=Path('stack_profile_tracker_empty_callstack.textproto'),
        query=Path('stack_profile_tracker_empty_callstack_test.sql'),
        out=Csv("""
"count"
0
"""))

  def test_unsymbolized_frames(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_no_symbols.textproto'),
        query=Metric('unsymbolized_frames'),
        out=Path('unsymbolized_frames.out'))

  def test_simpleperf_event(self):
    return DiffTestBlueprint(
        trace=Path('simpleperf_event.py'),
        query=Metric('android_simpleperf'),
        out=Path('simpleperf_event.out'))

  def test_java_heap_stats(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph.textproto'),
        query=Metric('java_heap_stats'),
        out=Path('java_heap_stats.out'))

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
        query=Path('perf_sample_test.sql'),
        out=Path('perf_sample_rvc.out'))

  def test_perf_sample_sc(self):
    return DiffTestBlueprint(
        trace=Path('../../data/perf_sample_sc.pb'),
        query=Path('perf_sample_test.sql'),
        out=Path('perf_sample_sc.out'))

  def test_stack_profile_symbols(self):
    return DiffTestBlueprint(
        trace=Path('../../data/heapprofd_standalone_client_example-trace'),
        query=Path('stack_profile_symbols_test.sql'),
        out=Path('stack_profile_symbols.out'))

  def test_callstack_sampling_flamegraph(self):
    return DiffTestBlueprint(
        trace=Path('../../data/callstack_sampling.pftrace'),
        query=Path('callstack_sampling_flamegraph_test.sql'),
        out=Path('callstack_sampling_flamegraph.out'))

  def test_callstack_sampling_flamegraph_multi_process(self):
    return DiffTestBlueprint(
        trace=Path('../../data/callstack_sampling.pftrace'),
        query=Path('callstack_sampling_flamegraph_multi_process_test.sql'),
        out=Csv("""
"count","description"
658,"BothProcesses"
483,"FirstProcess"
175,"SecondProcess"
"""))

  def test_no_build_id(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_data_local_tmp.textproto'),
        query=Path('no_build_id_test.sql'),
        out=Csv("""
"value"
1
"""))
