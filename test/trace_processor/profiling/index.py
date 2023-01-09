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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Profiling(DiffTestModule):

  def test_heap_profile_jit(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_jit.textproto'),
        query=Path('heap_profile_frames_test.sql'),
        out=Path('heap_profile_jit.out'))

  def test_heap_profile_deobfuscate(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_deobfuscate.textproto'),
        query=Path('heap_profile_deobfuscate_test.sql'),
        out=Path('heap_profile_deobfuscate.out'))

  def test_heap_profile_deobfuscate_2(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_deobfuscate_memfd.textproto'),
        query=Path('heap_profile_deobfuscate_test.sql'),
        out=Path('heap_profile_deobfuscate.out'))

  def test_heap_profile_dump_max_legacy(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_dump_max_legacy.textproto'),
        query=Path('heap_profile_tracker_new_stack_test.sql'),
        out=Path('heap_profile_dump_max_legacy.out'))

  def test_heap_profile_dump_max(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_dump_max.textproto'),
        query=Path('heap_profile_tracker_new_stack_test.sql'),
        out=Path('heap_profile_dump_max.out'))

  def test_profiler_smaps(self):
    return DiffTestBlueprint(
        trace=Path('profiler_smaps.textproto'),
        query=Path('profiler_smaps_test.sql'),
        out=Path('profiler_smaps.out'))

  def test_profiler_smaps_metric(self):
    return DiffTestBlueprint(
        trace=Path('profiler_smaps.textproto'),
        query=Path('profiler_smaps'),
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
        out=Path('heap_profile_tracker_new_stack.out'))

  def test_heap_profile_tracker_twoheaps(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_tracker_twoheaps.textproto'),
        query=Path('heap_profile_tracker_twoheaps_test.sql'),
        out=Path('heap_profile_tracker_twoheaps.out'))

  def test_heap_graph_flamegraph_focused(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_branching.textproto'),
        query=Path('heap_graph_flamegraph_focused_test.sql'),
        out=Path('heap_graph_flamegraph_focused.out'))

  def test_heap_graph_superclass(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_superclass.textproto'),
        query=Path('heap_graph_superclass_test.sql'),
        out=Path('heap_graph_superclass.out'))

  def test_heap_graph_native_size(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_native_size.textproto'),
        query=Path('heap_graph_native_size_test.sql'),
        out=Path('heap_graph_native_size.out'))

  def test_heap_graph_flamegraph_matches_objects(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_huge_size.textproto'),
        query=Path('heap_graph_flamegraph_matches_objects_test.sql'),
        out=Path('heap_graph_flamegraph_matches_objects.out'))

  def test_heap_graph_flamegraph_3(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_legacy.textproto'),
        query=Path('heap_graph_flamegraph_test.sql'),
        out=Path('heap_graph_flamegraph.out'))

  def test_stack_profile_tracker_empty_callstack(self):
    return DiffTestBlueprint(
        trace=Path('stack_profile_tracker_empty_callstack.textproto'),
        query=Path('stack_profile_tracker_empty_callstack_test.sql'),
        out=Path('stack_profile_tracker_empty_callstack.out'))

  def test_unsymbolized_frames(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_no_symbols.textproto'),
        query=Path('unsymbolized_frames'),
        out=Path('unsymbolized_frames.out'))

  def test_simpleperf_event(self):
    return DiffTestBlueprint(
        trace=Path('simpleperf_event.py'),
        query=Path('android_simpleperf'),
        out=Path('simpleperf_event.out'))

  def test_java_heap_stats(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph.textproto'),
        query=Path('java_heap_stats'),
        out=Path('java_heap_stats.out'))

  def test_heap_stats_closest_proc(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_closest_proc.textproto'),
        query=Path('java_heap_stats'),
        out=Path('heap_stats_closest_proc.out'))

  def test_java_heap_histogram(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph.textproto'),
        query=Path('java_heap_histogram'),
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
        out=Path('callstack_sampling_flamegraph_multi_process.out'))

  def test_no_build_id(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_data_local_tmp.textproto'),
        query=Path('no_build_id_test.sql'),
        out=Path('no_build_id.out'))
