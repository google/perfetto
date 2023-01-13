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
        out=Path('scroll_jank_cause.out'))

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
        out=Path('chrome_thread_slice.out'))

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
        out=Path('chrome_scroll_inputs_per_frame_test.out'))

  def test_chrome_thread_slice_repeated(self):
    return DiffTestBlueprint(
        trace=Path('../track_event/track_event_counters.textproto'),
        query=Path('chrome_thread_slice_repeated_test.sql'),
        out=Path('chrome_thread_slice_repeated.out'))

  def test_frame_times_metric(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_rendering_desktop.pftrace'),
        query=Metric('frame_times'),
        out=Path('frame_times_metric.out'))

  def test_chrome_dropped_frames_metric(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_rendering_desktop.pftrace'),
        query=Metric('chrome_dropped_frames'),
        out=Path('chrome_dropped_frames_metric.out'))

  def test_chrome_long_latency_metric(self):
    return DiffTestBlueprint(
        trace=Path('../chrome/long_event_latency.textproto'),
        query=Path('chrome_long_latency_metric_test.sql'),
        out=Path('chrome_long_latency_metric.out'))

  def test_scroll_jank_mojo_simple_watcher(self):
    return DiffTestBlueprint(
        trace=Path('scroll_jank_mojo_simple_watcher.py'),
        query=Path('scroll_jank_mojo_simple_watcher_test.sql'),
        out=Path('scroll_jank_mojo_simple_watcher.out'))

  def test_scroll_jank_gpu_check(self):
    return DiffTestBlueprint(
        trace=Path('scroll_jank_gpu_check.py'),
        query=Path('scroll_jank_gpu_check_test.sql'),
        out=Path('scroll_jank_gpu_check.out'))

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
        out=Path('touch_jank_synth.out'))

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
        out=Path('combined_rail_modes.out'))

  def test_cpu_time_by_combined_rail_mode(self):
    return DiffTestBlueprint(
        trace=Path('cpu_time_by_combined_rail_mode.py'),
        query=Path('cpu_time_by_combined_rail_mode_test.sql'),
        out=Path('cpu_time_by_combined_rail_mode.out'))

  def test_actual_power_by_combined_rail_mode(self):
    return DiffTestBlueprint(
        trace=Path('actual_power_by_combined_rail_mode.py'),
        query=Path('actual_power_by_combined_rail_mode_test.sql'),
        out=Path('actual_power_by_combined_rail_mode.out'))

  def test_estimated_power_by_combined_rail_mode(self):
    return DiffTestBlueprint(
        trace=Path('estimated_power_by_combined_rail_mode.py'),
        query=Path('estimated_power_by_combined_rail_mode_test.sql'),
        out=Path('estimated_power_by_combined_rail_mode.out'))

  def test_modified_rail_modes(self):
    return DiffTestBlueprint(
        trace=Path('modified_rail_modes.py'),
        query=Path('modified_rail_modes_test.sql'),
        out=Path('modified_rail_modes.out'))

  def test_modified_rail_modes_no_vsyncs(self):
    return DiffTestBlueprint(
        trace=Path('modified_rail_modes_no_vsyncs.py'),
        query=Path('modified_rail_modes_test.sql'),
        out=Path('modified_rail_modes_no_vsyncs.out'))

  def test_modified_rail_modes_with_input(self):
    return DiffTestBlueprint(
        trace=Path('modified_rail_modes_with_input.py'),
        query=Path('modified_rail_modes_with_input_test.sql'),
        out=Path('modified_rail_modes_with_input.out'))

  def test_modified_rail_modes_long(self):
    return DiffTestBlueprint(
        trace=Path('modified_rail_modes_long.py'),
        query=Path('modified_rail_modes_test.sql'),
        out=Path('modified_rail_modes_long.out'))

  def test_modified_rail_modes_extra_long(self):
    return DiffTestBlueprint(
        trace=Path('modified_rail_modes_extra_long.py'),
        query=Path('modified_rail_modes_test.sql'),
        out=Path('modified_rail_modes_extra_long.out'))

  def test_chrome_processes(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('chrome_processes_test.sql'),
        out=Path('chrome_processes.out'))

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
        out=Path('chrome_processes_type.out'))

  def test_chrome_processes_type_android_systrace(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_android_systrace.pftrace'),
        query=Path('chrome_processes_type_test.sql'),
        out=Path('chrome_processes_type_android_systrace.out'))

  def test_track_with_chrome_process(self):
    return DiffTestBlueprint(
        trace=Path('track_with_chrome_process.textproto'),
        query=Path('chrome_processes_type_test.sql'),
        out=Path('track_with_chrome_process.out'))

  def test_chrome_histogram_hashes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_histogram_hashes.textproto'),
        query=Metric('chrome_histogram_hashes'),
        out=Path('chrome_histogram_hashes.out'))

  def test_chrome_user_event_hashes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_user_event_hashes.textproto'),
        query=Metric('chrome_user_event_hashes'),
        out=Path('chrome_user_event_hashes.out'))

  def test_chrome_performance_mark_hashes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_performance_mark_hashes.textproto'),
        query=Metric('chrome_performance_mark_hashes'),
        out=Path('chrome_performance_mark_hashes.out'))

  def test_chrome_reliable_range(self):
    return DiffTestBlueprint(
        trace=Path('chrome_reliable_range.textproto'),
        query=Path('chrome_reliable_range_test.sql'),
        out=Path('chrome_reliable_range.out'))

  def test_chrome_reliable_range_cropping(self):
    return DiffTestBlueprint(
        trace=Path('chrome_reliable_range_cropping.textproto'),
        query=Path('chrome_reliable_range_test.sql'),
        out=Path('chrome_reliable_range_cropping.out'))

  def test_chrome_reliable_range_missing_processes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_reliable_range_missing_processes.textproto'),
        query=Path('chrome_reliable_range_test.sql'),
        out=Path('chrome_reliable_range_missing_processes.out'))

  def test_chrome_slice_names(self):
    return DiffTestBlueprint(
        trace=Path('chrome_slice_names.textproto'),
        query=Metric('chrome_slice_names'),
        out=Path('chrome_slice_names.out'))

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
        out=Path('unsymbolized_args.out'))

  def test_async_trace_1_count_slices(self):
    return DiffTestBlueprint(
        trace=Path('../../data/async-trace-1.json'),
        query=Path('count_slices_test.sql'),
        out=Path('async-trace-1_count_slices.out'))

  def test_async_trace_2_count_slices(self):
    return DiffTestBlueprint(
        trace=Path('../../data/async-trace-2.json'),
        query=Path('count_slices_test.sql'),
        out=Path('async-trace-2_count_slices.out'))

  def test_chrome_args_class_names(self):
    return DiffTestBlueprint(
        trace=Path('chrome_args_class_names.textproto'),
        query=Metric('chrome_args_class_names'),
        out=Path('chrome_args_class_names.out'))

  def test_chrome_log_message(self):
    return DiffTestBlueprint(
        trace=Path('chrome_log_message.textproto'),
        query=Path('chrome_log_message_test.sql'),
        out=Path('chrome_log_message.out'))

  def test_chrome_log_message_args(self):
    return DiffTestBlueprint(
        trace=Path('chrome_log_message.textproto'),
        query=Path('chrome_log_message_args_test.sql'),
        out=Path('chrome_log_message_args.out'))

  def test_chrome_missing_processes_default_trace(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('chrome_missing_processes_test.sql'),
        out=Path('chrome_missing_processes_default_trace.out'))

  def test_chrome_missing_processes(self):
    return DiffTestBlueprint(
        trace=Path('chrome_missing_processes.textproto'),
        query=Path('chrome_missing_processes_test.sql'),
        out=Path('chrome_missing_processes.out'))

  def test_chrome_missing_processes_args(self):
    return DiffTestBlueprint(
        trace=Path('chrome_missing_processes.textproto'),
        query=Path('chrome_missing_processes_args_test.sql'),
        out=Path('chrome_missing_processes_args.out'))

  def test_chrome_missing_processes_2(self):
    return DiffTestBlueprint(
        trace=Path('chrome_missing_processes_extension.textproto'),
        query=Path('chrome_missing_processes_test.sql'),
        out=Path('chrome_missing_processes.out'))

  def test_chrome_missing_processes_extension_args(self):
    return DiffTestBlueprint(
        trace=Path('chrome_missing_processes_extension.textproto'),
        query=Path('chrome_missing_processes_args_test.sql'),
        out=Path('chrome_missing_processes_extension_args.out'))

  def test_chrome_custom_navigation_tasks(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_custom_navigation_trace.gz'),
        query=Path('chrome_custom_navigation_tasks_test.sql'),
        out=Path('chrome_custom_navigation_tasks.out'))

  def test_proto_content(self):
    return DiffTestBlueprint(
        trace=Path('../../data/chrome_scroll_without_vsync.pftrace'),
        query=Path('proto_content_test.sql'),
        out=Path('proto_content.out'))
