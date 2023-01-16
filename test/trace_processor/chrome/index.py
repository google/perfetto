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


class DiffTestModule_Chrome(DiffTestModule):

  def test_scroll_jank_general_validation(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('scroll_jank_general_validation_test.sql'),
        out=Path('scroll_jank_general_validation.out'))

  def test_scroll_jank(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('scroll_jank_test.sql'),
        out=Path('scroll_jank.out'))

  def test_event_latency_to_breakdowns(self):
    return DiffTestBlueprint(
        trace=Path('../../data/event_latency_with_args.perfetto-trace'),
        query=Path('event_latency_to_breakdowns_test.sql'),
        out=Path('event_latency_to_breakdowns.out'))

  def test_event_latency_scroll_jank(self):
    return DiffTestBlueprint(
        trace=Path('../../data/event_latency_with_args.perfetto-trace'),
        query=Path('event_latency_scroll_jank_test.sql'),
        out=Path('event_latency_scroll_jank.out'))

  def test_event_latency_scroll_jank_cause(self):
    return DiffTestBlueprint(
        trace=Path('../../data/event_latency_with_args.perfetto-trace'),
        query=Path('event_latency_scroll_jank_cause_test.sql'),
        out=Path('event_latency_scroll_jank_cause.out'))

  def test_scroll_flow_event(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('scroll_flow_event_test.sql'),
        out=Path('scroll_flow_event.out'))

  def test_scroll_flow_event_general_validation(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('scroll_flow_event_general_validation_test.sql'),
        out=Path('scroll_flow_event_general_validation.out'))

  def test_scroll_jank_cause(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('scroll_jank_cause_test.sql'),
        out=Csv("""
"total","total_jank","sum_explained_and_unexplained","error_rows"
139,7,7,0
"""))

  def test_scroll_flow_event_queuing_delay(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('scroll_flow_event_queuing_delay_test.sql'),
        out=Path('scroll_flow_event_queuing_delay.out'))

  def test_scroll_flow_event_general_validation_2(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path(
            'scroll_flow_event_queuing_delay_general_validation_test.sql'),
        out=Path('scroll_flow_event_general_validation.out'))

  def test_scroll_jank_cause_queuing_delay(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('scroll_jank_cause_queuing_delay_test.sql'),
        out=Path('scroll_jank_cause_queuing_delay.out'))

  def test_scroll_jank_cause_queuing_delay_restricted(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('scroll_jank_cause_queuing_delay_restricted_test.sql'),
        out=Path('scroll_jank_cause_queuing_delay_restricted.out'))

  def test_scroll_jank_cause_queuing_delay_general_validation(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path(
            'scroll_jank_cause_queuing_delay_general_validation_test.sql'),
        out=Path('scroll_jank_cause_queuing_delay_general_validation.out'))

  def test_chrome_thread_slice(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('chrome_thread_slice_test.sql'),
        out=Csv("""
"trace_id","dur","thread_dur"
2734,25000,25000
2734,1000,2000
2734,2000,2000
2734,258000,171000
2734,1000,1000
"""))

  def test_chrome_input_to_browser_intervals(self):
    return DiffTestBlueprint(
        trace=Path(
            '../../data/scrolling_with_blocked_nonblocked_frames.pftrace'),
        query=Path('chrome_input_to_browser_intervals_test.sql'),
        out=Path('chrome_input_to_browser_intervals.out'))

  def test_chrome_scroll_jank_caused_by_scheduling_test(self):
    return DiffTestBlueprint(
        trace=Path('../../data/fling_with_input_delay.pftrace'),
        query=Path('chrome_scroll_jank_caused_by_scheduling_test.sql'),
        out=Path('chrome_scroll_jank_caused_by_scheduling_test.out'))

  def test_chrome_tasks_delaying_input_processing_test(self):
    return DiffTestBlueprint(
        trace=Path('../../data/fling_with_input_delay.pftrace'),
        query=Path('chrome_tasks_delaying_input_processing_test.sql'),
        out=Path('chrome_tasks_delaying_input_processing_test.out'))

  def test_long_task_tracking_trace_chrome_long_tasks_delaying_input_processing_test(
      self):
    return DiffTestBlueprint(
        trace=Path('../../data/long_task_tracking_trace'),
        query=Path('chrome_long_tasks_delaying_input_processing_test.sql'),
        out=Path(
            'long_task_tracking_trace_chrome_long_tasks_delaying_input_processing_test.out'
        ))

  def test_experimental_reliable_chrome_tasks_delaying_input_processing_test(
      self):
    return DiffTestBlueprint(
        trace=Path('../../data/fling_with_input_delay.pftrace'),
        query=Path(
            'experimental_reliable_chrome_tasks_delaying_input_processing_test.sql'
        ),
        out=Path(
            'experimental_reliable_chrome_tasks_delaying_input_processing_test.out'
        ))

  def test_chrome_scroll_inputs_per_frame_test(self):
    return DiffTestBlueprint(
        trace=Path(
            '../../data/scrolling_with_blocked_nonblocked_frames.pftrace'),
        query=Path('chrome_scroll_inputs_per_frame_test.sql'),
        out=Csv("""
"count_for_frame","ts"
4,60934316798158
"""))

  def test_chrome_thread_slice_repeated(self):
    return DiffTestBlueprint(
        trace=Path('../track_event/track_event_counters.textproto'),
        query=Path('chrome_thread_slice_repeated_test.sql'),
        out=Csv("""
"name","ts","dur","thread_dur"
"event1_on_t1",1000,100,10000
"event2_on_t1",2000,200,30000
"event3_on_t1",2000,200,10000
"event4_on_t1",4000,0,0
"float_counter_on_t1",4300,0,"[NULL]"
"float_counter_on_t1",4500,0,"[NULL]"
"event1_on_t3",4000,100,5000
"""))

  def test_frame_times_metric(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_rendering_desktop.pftrace'),
        query=Metric('frame_times'),
        out=Path('frame_times_metric.out'))

  def test_chrome_dropped_frames_metric(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_rendering_desktop.pftrace'),
        query=Metric('chrome_dropped_frames'),
        out=TextProto(r"""
[perfetto.protos.chrome_dropped_frames]: {
  dropped_frame: {
    ts: 166479338462000
    process_name: "Renderer"
    pid: 12743
  }
  dropped_frame: {
    ts: 166479355302000
    process_name: "Renderer"
    pid: 12743
  }
}"""))

  def test_chrome_long_latency_metric(self):
    return DiffTestBlueprint(
        trace=Path('../chrome/long_event_latency.textproto'),
        query=Path('chrome_long_latency_metric_test.sql'),
        out=Csv("""
"ts","event_type","process_name","process_id"
200111000,"FirstGestureScrollUpdate,GestureScrollUpdate","Renderer",1001
200111000,"GestureScrollUpdate","Renderer",1002
280111001,"GestureScrollUpdate","Renderer",1001
"""))

  def test_scroll_jank_mojo_simple_watcher(self):
    return DiffTestBlueprint(
        trace=Path('scroll_jank_mojo_simple_watcher.py'),
        query=Path('scroll_jank_mojo_simple_watcher_test.sql'),
        out=Path('scroll_jank_mojo_simple_watcher.out'))

  def test_scroll_jank_gpu_check(self):
    return DiffTestBlueprint(
        trace=Path('scroll_jank_gpu_check.py'),
        query=Path('scroll_jank_gpu_check_test.sql'),
        out=Csv("""
"ts","jank"
15000000,0
30000000,1
115000000,0
"""))

  def test_touch_jank(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_touch_gesture_scroll.pftrace'),
        query=Path('touch_jank_test.sql'),
        out=Path('touch_jank.out'))

  def test_touch_flow_event(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_touch_gesture_scroll.pftrace'),
        query=Path('touch_flow_event_test.sql'),
        out=Path('touch_flow_event.out'))

  def test_touch_flow_event_queuing_delay(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_touch_gesture_scroll.pftrace'),
        query=Path('touch_flow_event_queuing_delay_test.sql'),
        out=Path('touch_flow_event_queuing_delay.out'))

  def test_touch_jank_synth(self):
    return DiffTestBlueprint(
        trace=Path('touch_jank.py'),
        query=Path('touch_jank_test.sql'),
        out=Csv("""
"touch_id","trace_id","jank","ts","dur","jank_budget"
87654,34577,0,0,10000000,-31333333.350000
87654,34578,1,16000000,33000000,14666666.650000
87654,34579,0,55000000,33000000,-8333333.350000
"""))

  def test_touch_flow_event_synth(self):
    return DiffTestBlueprint(
        trace=Path('touch_jank.py'),
        query=Path('touch_flow_event_test.sql'),
        out=Path('touch_flow_event_synth.out'))

  def test_touch_flow_event_queuing_delay_synth(self):
    return DiffTestBlueprint(
        trace=Path('touch_jank.py'),
        query=Path('touch_flow_event_queuing_delay_full_test.sql'),
        out=Path('touch_flow_event_queuing_delay_synth.out'))

  def test_memory_snapshot_general_validation(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_memory_snapshot.pftrace'),
        query=Path('memory_snapshot_general_validation_test.sql'),
        out=Path('memory_snapshot_general_validation.out'))

  def test_memory_snapshot_os_dump_events(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_memory_snapshot.pftrace'),
        query=Path('memory_snapshot_os_dump_events_test.sql'),
        out=Path('memory_snapshot_os_dump_events.out'))

  def test_memory_snapshot_chrome_dump_events(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_memory_snapshot.pftrace'),
        query=Path('memory_snapshot_chrome_dump_events_test.sql'),
        out=Path('memory_snapshot_chrome_dump_events.out'))

  def test_memory_snapshot_nodes(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_memory_snapshot.pftrace'),
        query=Path('memory_snapshot_nodes_test.sql'),
        out=Path('memory_snapshot_nodes.out'))

  def test_memory_snapshot_edges(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_memory_snapshot.pftrace'),
        query=Path('memory_snapshot_edges_test.sql'),
        out=Path('memory_snapshot_edges.out'))

  def test_memory_snapshot_node_args(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_memory_snapshot.pftrace'),
        query=Path('memory_snapshot_node_args_test.sql'),
        out=Path('memory_snapshot_node_args.out'))

  def test_memory_snapshot_smaps(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_memory_snapshot.pftrace'),
        query=Path('memory_snapshot_smaps_test.sql'),
        out=Path('memory_snapshot_smaps.out'))

  def test_combined_rail_modes(self):
    return DiffTestBlueprint(
        trace=Path('combined_rail_modes.py'),
        query=Path('combined_rail_modes_test.sql'),
        out=Csv("""
"id","ts","dur","rail_mode"
1,0,10000,"response"
2,10000,25000,"animation"
3,35000,10000,"background"
"""))

  def test_cpu_time_by_combined_rail_mode(self):
    return DiffTestBlueprint(
        trace=Path('cpu_time_by_combined_rail_mode.py'),
        query=Path('cpu_time_by_combined_rail_mode_test.sql'),
        out=Csv("""
"id","ts","dur","rail_mode","cpu_dur"
1,0,10000,"response",26000
2,10000,20000,"animation",20000
3,30000,5000,"background",8000
4,35000,10000,"animation",21000
5,45000,10000,"background",1000
"""))

  def test_actual_power_by_combined_rail_mode(self):
    return DiffTestBlueprint(
        trace=Path('actual_power_by_combined_rail_mode.py'),
        query=Path('actual_power_by_combined_rail_mode_test.sql'),
        out=Csv("""
"id","ts","dur","rail_mode","subsystem","joules","drain_w"
1,0,10000000,"response","cellular",0.000000,0.000000
1,0,10000000,"response","cpu_little",0.000140,0.014000
2,10000000,20000000,"animation","cellular",0.000350,0.017500
2,10000000,20000000,"animation","cpu_little",0.000140,0.007000
3,30000000,5000000,"background","cellular",0.000018,0.003500
3,30000000,5000000,"background","cpu_little",0.000007,0.001400
4,35000000,10000000,"animation","cellular",0.000021,0.002100
4,35000000,10000000,"animation","cpu_little",0.000070,0.007000
5,45000000,10000000,"background","cellular",0.000003,0.000350
5,45000000,10000000,"background","cpu_little",0.000070,0.007000
"""))

  def test_estimated_power_by_combined_rail_mode(self):
    return DiffTestBlueprint(
        trace=Path('estimated_power_by_combined_rail_mode.py'),
        query=Path('estimated_power_by_combined_rail_mode_test.sql'),
        out=Csv("""
"id","ts","dur","rail_mode","mas","ma"
1,0,10000000,"response",0.554275,55.427500
2,10000000,20000000,"animation",0.284850,14.242500
3,30000000,5000000,"background",0.076233,15.246667
4,35000000,10000000,"animation",0.536850,53.685000
5,45000000,10000000,"background",0.071580,7.158000
"""))

  def test_modified_rail_modes(self):
    return DiffTestBlueprint(
        trace=Path('modified_rail_modes.py'),
        query=Path('modified_rail_modes_test.sql'),
        out=Csv("""
"id","ts","dur","mode"
2,0,1000000000,"response"
3,1000000000,1950000000,"foreground_idle"
4,2950000000,333333324,"animation"
5,3283333324,216666676,"foreground_idle"
6,3500000000,1000000000,"background"
"""))

  def test_modified_rail_modes_no_vsyncs(self):
    return DiffTestBlueprint(
        trace=Path('modified_rail_modes_no_vsyncs.py'),
        query=Path('modified_rail_modes_test.sql'),
        out=Csv("""
"id","ts","dur","mode"
2,0,1000000000,"response"
3,1000000000,2500000000,"foreground_idle"
4,3500000000,1000000000,"background"
"""))

  def test_modified_rail_modes_with_input(self):
    return DiffTestBlueprint(
        trace=Path('modified_rail_modes_with_input.py'),
        query=Path('modified_rail_modes_with_input_test.sql'),
        out=Csv("""
"id","ts","dur","mode"
2,0,1000000000,"response"
3,1000000000,1950000000,"foreground_idle"
4,2950000000,50000000,"animation"
5,3000000000,66666674,"response"
6,3066666674,216666650,"animation"
7,3283333324,216666676,"foreground_idle"
8,3500000000,1000000000,"background"
"""))

  def test_modified_rail_modes_long(self):
    return DiffTestBlueprint(
        trace=Path('modified_rail_modes_long.py'),
        query=Path('modified_rail_modes_test.sql'),
        out=Csv("""
"id","ts","dur","mode"
2,0,1000000000,"response"
3,1000000000,1,"background"
"""))

  def test_modified_rail_modes_extra_long(self):
    return DiffTestBlueprint(
        trace=Path('modified_rail_modes_extra_long.py'),
        query=Path('modified_rail_modes_test.sql'),
        out=Csv("""
"id","ts","dur","mode"
"""))

  def test_chrome_processes(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('chrome_processes_test.sql'),
        out=Csv("""
"pid","name","process_type"
18250,"Renderer","Renderer"
17547,"Browser","Browser"
18277,"GPU Process","Gpu"
17578,"Browser","Browser"
"""))

  def test_chrome_processes_android_systrace(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_android_systrace.pftrace'),
        query=Path('chrome_processes_test.sql'),
        out=Path('chrome_processes_android_systrace.out'))

  def test_chrome_threads(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('chrome_threads_test.sql'),
        out=Path('chrome_threads.out'))

  def test_chrome_threads_android_systrace(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_android_systrace.pftrace'),
        query=Path('chrome_threads_test.sql'),
        out=Path('chrome_threads_android_systrace.out'))

  def test_chrome_processes_type(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('chrome_processes_type_test.sql'),
        out=Csv("""
"pid","name","chrome_process_type"
17547,"Browser","Browser"
17578,"Browser","Browser"
18250,"Renderer","Renderer"
18277,"GPU Process","Gpu"
"""))

  def test_chrome_processes_type_android_systrace(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_android_systrace.pftrace'),
        query=Path('chrome_processes_type_test.sql'),
        out=Path('chrome_processes_type_android_systrace.out'))

  def test_track_with_chrome_process(self):
    return DiffTestBlueprint(
        trace=Path('track_with_chrome_process.textproto'),
        query=Path('chrome_processes_type_test.sql'),
        out=Csv("""
"pid","name","chrome_process_type"
5,"p5","[NULL]"
"""))

  def test_chrome_histogram_hashes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_histogram_hashes.textproto'),
        query=Metric('chrome_histogram_hashes'),
        out=TextProto(r"""
[perfetto.protos.chrome_histogram_hashes]: {
  hash: 10
  hash: 20
}
"""))

  def test_chrome_user_event_hashes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_user_event_hashes.textproto'),
        query=Metric('chrome_user_event_hashes'),
        out=TextProto(r"""
[perfetto.protos.chrome_user_event_hashes]: {
  action_hash: 10
  action_hash: 20
}

"""))

  def test_chrome_performance_mark_hashes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_performance_mark_hashes.textproto'),
        query=Metric('chrome_performance_mark_hashes'),
        out=TextProto(r"""
[perfetto.protos.chrome_performance_mark_hashes]: {
  site_hash: 10
  site_hash: 20
  mark_hash: 100
  mark_hash: 200
}
"""))

  def test_chrome_reliable_range(self):
    return DiffTestBlueprint(
        trace=Path('chrome_reliable_range.textproto'),
        query=Path('chrome_reliable_range_test.sql'),
        out=Csv("""
"start","reason","debug_limiting_upid","debug_limiting_utid"
12,"First slice for utid=2","[NULL]",2
"""))

  def test_chrome_reliable_range_cropping(self):
    return DiffTestBlueprint(
        trace=Path('chrome_reliable_range_cropping.textproto'),
        query=Path('chrome_reliable_range_test.sql'),
        out=Csv("""
"start","reason","debug_limiting_upid","debug_limiting_utid"
10000,"Range of interest packet","[NULL]",2
"""))

  def test_chrome_reliable_range_missing_processes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_reliable_range_missing_processes.textproto'),
        query=Path('chrome_reliable_range_test.sql'),
        out=Csv("""
"start","reason","debug_limiting_upid","debug_limiting_utid"
1011,"Missing process data for upid=2",2,1
"""))

  def test_chrome_slice_names(self):
    return DiffTestBlueprint(
        trace=Path('chrome_slice_names.textproto'),
        query=Metric('chrome_slice_names'),
        out=TextProto(r"""
[perfetto.protos.chrome_slice_names]: {
  chrome_version_code: 123
  slice_name: "Looper.Dispatch: class1"
  slice_name: "name2"
}
"""))

  def test_chrome_tasks(self):
    return DiffTestBlueprint(
        trace=Path(
            '../../data/chrome_page_load_all_categories_not_extended.pftrace.gz'
        ),
        query=Path('chrome_tasks_test.sql'),
        out=Path('chrome_tasks.out'))

  def test_top_level_java_choreographer_slices_top_level_java_chrome_tasks_test(
      self):
    return DiffTestBlueprint(
        trace=Path('../../data/top_level_java_choreographer_slices'),
        query=Path('top_level_java_chrome_tasks_test.sql'),
        out=Path(
            'top_level_java_choreographer_slices_top_level_java_chrome_tasks_test.out'
        ))

  def test_chrome_stack_samples_for_task_test(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_stack_traces_symbolized_trace.pftrace'),
        query=Path('chrome_stack_samples_for_task_test.sql'),
        out=Path('chrome_stack_samples_for_task_test.out'))

  def test_unsymbolized_args(self):
    return DiffTestBlueprint(
        trace=Path('unsymbolized_args.textproto'),
        query=Metric('chrome_unsymbolized_args'),
        out=TextProto(r"""
[perfetto.protos.chrome_unsymbolized_args]: {
  args {
     module: "/liblib.so"
     build_id: "6275696c642d6964"
     address: 123
     google_lookup_id: "6275696c642d6964"
   }
   args {
     module: "/libmonochrome_64.so"
     build_id: "7f0715c286f8b16c10e4ad349cda3b9b56c7a773"
     address: 234
     google_lookup_id: "c215077ff8866cb110e4ad349cda3b9b0"
   }
}"""))

  def test_async_trace_1_count_slices(self):
    return DiffTestBlueprint(
        trace=Path('../../data/async-trace-1.json'),
        query=Path('count_slices_test.sql'),
        out=Csv("""
"COUNT(1)"
16
"""))

  def test_async_trace_2_count_slices(self):
    return DiffTestBlueprint(
        trace=Path('../../data/async-trace-2.json'),
        query=Path('count_slices_test.sql'),
        out=Csv("""
"COUNT(1)"
35
"""))

  def test_chrome_args_class_names(self):
    return DiffTestBlueprint(
        trace=Path('chrome_args_class_names.textproto'),
        query=Metric('chrome_args_class_names'),
        out=TextProto(r"""

[perfetto.protos.chrome_args_class_names] {
  class_names_per_version {
    class_name: "abc"
    class_name: "def"
    class_name: "ghi"
    class_name: "jkl"
  }
}
"""))

  def test_chrome_log_message(self):
    return DiffTestBlueprint(
        trace=Path('chrome_log_message.textproto'),
        query=Path('chrome_log_message_test.sql'),
        out=Csv("""
"utid","tag","msg"
1,"foo.cc:123","log message"
"""))

  def test_chrome_log_message_args(self):
    return DiffTestBlueprint(
        trace=Path('chrome_log_message.textproto'),
        query=Path('chrome_log_message_args_test.sql'),
        out=Csv("""
"log_message","function_name","file_name","line_number"
"log message","func","foo.cc",123
"""))

  def test_chrome_missing_processes_default_trace(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('chrome_missing_processes_test.sql'),
        out=Csv("""
"upid","pid","reliable_from"
"""))

  def test_chrome_missing_processes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_missing_processes.textproto'),
        query=Path('chrome_missing_processes_test.sql'),
        out=Csv("""
"upid","pid","reliable_from"
2,100,1000000000
3,1000,"[NULL]"
"""))

  def test_chrome_missing_processes_args(self):
    return DiffTestBlueprint(
        trace=Path('chrome_missing_processes.textproto'),
        query=Path('chrome_missing_processes_args_test.sql'),
        out=Csv("""
"arg_set_id","key","int_value"
2,"chrome_active_processes.pid[0]",10
2,"chrome_active_processes.pid[1]",100
2,"chrome_active_processes.pid[2]",1000
"""))

  def test_chrome_missing_processes_2(self):
    return DiffTestBlueprint(
        trace=Path('chrome_missing_processes_extension.textproto'),
        query=Path('chrome_missing_processes_test.sql'),
        out=Csv("""
"upid","pid","reliable_from"
2,100,1000000000
3,1000,"[NULL]"
"""))

  def test_chrome_missing_processes_extension_args(self):
    return DiffTestBlueprint(
        trace=Path('chrome_missing_processes_extension.textproto'),
        query=Path('chrome_missing_processes_args_test.sql'),
        out=Csv("""
"arg_set_id","key","int_value"
2,"active_processes.pid[0]",10
2,"active_processes.pid[1]",100
2,"active_processes.pid[2]",1000
"""))

  def test_chrome_custom_navigation_tasks(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_custom_navigation_trace.gz'),
        query=Path('chrome_custom_navigation_tasks_test.sql'),
        out=Csv("""
"full_name","task_type","count"
"FrameHost::BeginNavigation (SUBFRAME)","navigation_task",5
"FrameHost::DidStopLoading (SUBFRAME)","navigation_task",3
"FrameHost::BeginNavigation (PRIMARY_MAIN_FRAME)","navigation_task",1
"FrameHost::DidCommitProvisionalLoad (SUBFRAME)","navigation_task",1
"""))

  def test_proto_content(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('proto_content_test.sql'),
        out=Path('proto_content.out'))
