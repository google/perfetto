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


class DiffTestModule_Parsing(DiffTestModule):

  def test_ts_desc_filter_android_sched_and_ps(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query=Path('ts_desc_filter_test.sql'),
        out=Path('ts_desc_filter_android_sched_and_ps.out'))

  def test_android_sched_and_ps_end_reason_eq(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query=Path('end_reason_eq_test.sql'),
        out=Path('android_sched_and_ps_end_reason_eq.out'))

  def test_android_sched_and_ps_end_reason_neq(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query=Path('end_reason_neq_test.sql'),
        out=Path('android_sched_and_ps_end_reason_neq.out'))

  def test_cpu_counters_b120487929(self):
    return DiffTestBlueprint(
        trace=Path('../../data/cpu_counters.pb'),
        query=Path('b120487929_test.sql'),
        out=Path('cpu_counters_b120487929.out'))

  def test_ftrace_with_tracing_start_list_sched_slice_spans(self):
    return DiffTestBlueprint(
        trace=Path('ftrace_with_tracing_start.py'),
        query=Path('list_sched_slice_spans_test.sql'),
        out=Path('ftrace_with_tracing_start_list_sched_slice_spans.out'))

  def test_rss_stat_mm_id(self):
    return DiffTestBlueprint(
        trace=Path('rss_stat_mm_id.py'),
        query=Path('rss_stat_test.sql'),
        out=Path('rss_stat_mm_id.out'))

  def test_rss_stat_mm_id_clone(self):
    return DiffTestBlueprint(
        trace=Path('rss_stat_mm_id_clone.py'),
        query=Path('rss_stat_test.sql'),
        out=Path('rss_stat_mm_id_clone.out'))

  def test_rss_stat_mm_id_reuse(self):
    return DiffTestBlueprint(
        trace=Path('rss_stat_mm_id_reuse.py'),
        query=Path('rss_stat_test.sql'),
        out=Path('rss_stat_mm_id_reuse.out'))

  def test_rss_stat_legacy(self):
    return DiffTestBlueprint(
        trace=Path('rss_stat_legacy.py'),
        query=Path('rss_stat_test.sql'),
        out=Path('rss_stat_legacy.out'))

  def test_rss_stat_after_free(self):
    return DiffTestBlueprint(
        trace=Path('rss_stat_after_free.py'),
        query=Path('rss_stat_after_free_test.sql'),
        out=Path('rss_stat_after_free.out'))

  def test_memory_counters_args_string_filter_null(self):
    return DiffTestBlueprint(
        trace=Path('../../data/memory_counters.pb'),
        query=Path('args_string_filter_null_test.sql'),
        out=Path('memory_counters_args_string_filter_null.out'))

  def test_memory_counters_args_string_is_null(self):
    return DiffTestBlueprint(
        trace=Path('../../data/memory_counters.pb'),
        query=Path('args_string_is_null_test.sql'),
        out=Path('memory_counters_args_string_is_null.out'))

  def test_memory_counters_args_string_is_not_null(self):
    return DiffTestBlueprint(
        trace=Path('../../data/memory_counters.pb'),
        query=Path('args_string_is_not_null_test.sql'),
        out=Path('memory_counters_args_string_is_not_null.out'))

  def test_memory_counters_b120605557(self):
    return DiffTestBlueprint(
        trace=Path('../../data/memory_counters.pb'),
        query=Path('b120605557_test.sql'),
        out=Path('memory_counters_b120605557.out'))

  def test_global_memory_counter_memory_counters(self):
    return DiffTestBlueprint(
        trace=Path('../../data/memory_counters.pb'),
        query=Path('global_memory_counter_test.sql'),
        out=Path('global_memory_counter_memory_counters.out'))

  def test_ion_stat(self):
    return DiffTestBlueprint(
        trace=Path('ion_stat.textproto'),
        query=Path('ion_stat_test.sql'),
        out=Path('ion_stat.out'))

  def test_sched_slices_sched_switch_original(self):
    return DiffTestBlueprint(
        trace=Path('../../data/sched_switch_original.pb'),
        query=Path('sched_slices_test.sql'),
        out=Path('sched_slices_sched_switch_original.out'))

  def test_sched_slices_sched_switch_compact(self):
    return DiffTestBlueprint(
        trace=Path('../../data/sched_switch_compact.pb'),
        query=Path('sched_slices_test.sql'),
        out=Path('sched_slices_sched_switch_compact.out'))

  def test_sched_waking_raw_compact_sched(self):
    return DiffTestBlueprint(
        trace=Path('../../data/compact_sched.pb'),
        query=Path('sched_waking_raw_test.sql'),
        out=Path('sched_waking_raw_compact_sched.out'))

  def test_sched_waking_instants_compact_sched(self):
    return DiffTestBlueprint(
        trace=Path('../../data/compact_sched.pb'),
        query=Path('sched_waking_instants_test.sql'),
        out=Path('sched_waking_instants_compact_sched.out'))

  def test_mm_event(self):
    return DiffTestBlueprint(
        trace=Path('../../data/mm_event.pb'),
        query=Path('mm_event_test.sql'),
        out=Path('mm_event.out'))

  def test_print_systrace_lmk_userspace(self):
    return DiffTestBlueprint(
        trace=Path('../../data/lmk_userspace.pb'),
        query=Path('print_systrace_test.sql'),
        out=Path('print_systrace_lmk_userspace.out'))

  def test_kernel_tmw_counter_process_counter_and_track(self):
    return DiffTestBlueprint(
        trace=Path('kernel_tmw_counter.textproto'),
        query=Path('process_counter_and_track_test.sql'),
        out=Path('kernel_tmw_counter_process_counter_and_track.out'))

  def test_kernel_dpu_tmw_counter_process_counter_and_track(self):
    return DiffTestBlueprint(
        trace=Path('kernel_dpu_tmw_counter.textproto'),
        query=Path('process_counter_and_track_test.sql'),
        out=Path('kernel_dpu_tmw_counter_process_counter_and_track.out'))

  def test_print_systrace_unsigned(self):
    return DiffTestBlueprint(
        trace=Path('print_systrace_unsigned.py'),
        query=Path('print_systrace_test.sql'),
        out=Path('print_systrace_unsigned.out'))

  def test_cgroup_attach_task_pre_s_print_systrace(self):
    return DiffTestBlueprint(
        trace=Path('cgroup_attach_task_pre_s.textproto'),
        query=Path('print_systrace_test.sql'),
        out=Path('cgroup_attach_task_pre_s_print_systrace.out'))

  def test_cgroup_attach_task_post_s_print_systrace(self):
    return DiffTestBlueprint(
        trace=Path('cgroup_attach_task_post_s.textproto'),
        query=Path('print_systrace_test.sql'),
        out=Path('cgroup_attach_task_post_s_print_systrace.out'))

  def test_systrace_html(self):
    return DiffTestBlueprint(
        trace=Path('../../data/systrace.html'),
        query=Path('systrace_html_test.sql'),
        out=Path('systrace_html.out'))

  def test_sched_smoke_trailing_empty(self):
    return DiffTestBlueprint(
        trace=Path('../../data/trailing_empty.systrace'),
        query=Path('sched_smoke_test.sql'),
        out=Path('sched_smoke_trailing_empty.out'))

  def test_lmk_userspace_lmk(self):
    return DiffTestBlueprint(
        trace=Path('../../data/lmk_userspace.pb'),
        query=Path('lmk_test.sql'),
        out=Path('lmk_userspace_lmk.out'))

  def test_oom_kill(self):
    return DiffTestBlueprint(
        trace=Path('../common/oom_kill.textproto'),
        query=Path('oom_kill_test.sql'),
        out=Path('oom_kill.out'))

  def test_android_log_counts(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_log.pb'),
        query=Path('android_log_counts_test.sql'),
        out=Path('android_log_counts.out'))

  def test_android_log_msgs(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_log.pb'),
        query=Path('android_log_msgs_test.sql'),
        out=Path('android_log_msgs.out'))

  def test_android_log_ring_buffer_mode(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_log_ring_buffer_mode.pb'),
        query=Path('android_log_ring_buffer_mode_test.sql'),
        out=Path('android_log_ring_buffer_mode.out'))

  def test_synth_oom_oom_query(self):
    return DiffTestBlueprint(
        trace=Path('synth_oom.py'),
        query=Path('oom_query_test.sql'),
        out=Path('synth_oom_oom_query.out'))

  def test_process_stats_poll_oom_score(self):
    return DiffTestBlueprint(
        trace=Path('../../data/process_stats_poll.pb'),
        query=Path('oom_score_poll_test.sql'),
        out=Path('process_stats_poll_oom_score.out'))

  def test_android_sched_and_ps_stats(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query=Path('stats_test.sql'),
        out=Path('android_sched_and_ps_stats.out'))

  def test_sys_syscall(self):
    return DiffTestBlueprint(
        trace=Path('syscall.py'),
        query=Path('sys_test.sql'),
        out=Path('sys_syscall.out'))

  def test_thread_time_in_thread_slice(self):
    return DiffTestBlueprint(
        trace=Path('flow_events_json_v2.json'),
        query=Path('thread_time_in_thread_slice_test.sql'),
        out=Path('thread_time_in_thread_slice.out'))

  def test_initial_display_state(self):
    return DiffTestBlueprint(
        trace=Path('initial_display_state.textproto'),
        query=Path('initial_display_state_test.sql'),
        out=Path('initial_display_state.out'))

  def test_config_metadata(self):
    return DiffTestBlueprint(
        trace=Path('config_metadata.textproto'),
        query=Path('metadata_test.sql'),
        out=Path('config_metadata.out'))

  def test_triggers_packets_trigger_packet_trace(self):
    return DiffTestBlueprint(
        trace=Path('trigger_packet_trace.textproto'),
        query=Path('triggers_packets_test.sql'),
        out=Path('triggers_packets_trigger_packet_trace.out'))

  def test_chrome_metadata(self):
    return DiffTestBlueprint(
        trace=Path('chrome_metadata.textproto'),
        query=Path('chrome_metadata_test.sql'),
        out=Path('chrome_metadata.out'))

  def test_cpu(self):
    return DiffTestBlueprint(
        trace=Path('cpu_info.textproto'),
        query=Path('cpu_test.sql'),
        out=Path('cpu.out'))

  def test_cpu_freq(self):
    return DiffTestBlueprint(
        trace=Path('cpu_info.textproto'),
        query=Path('cpu_freq_test.sql'),
        out=Path('cpu_freq.out'))

  def test_android_sched_and_ps_trace_size(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query=Path('trace_size_test.sql'),
        out=Path('android_sched_and_ps_trace_size.out'))

  def test_android_package_list(self):
    return DiffTestBlueprint(
        trace=Path('android_package_list.py'),
        query=Metric('android_package_list'),
        out=Path('android_package_list.out'))

  def test_process_metadata_matching(self):
    return DiffTestBlueprint(
        trace=Path('process_metadata_matching.textproto'),
        query=Path('process_metadata_matching_test.sql'),
        out=Path('process_metadata_matching.out'))

  def test_flow_events_json_v1(self):
    return DiffTestBlueprint(
        trace=Path('flow_events_json_v1.json'),
        query=Path('flow_events_test.sql'),
        out=Path('flow_events_json_v1.out'))

  def test_flow_events_json_v2(self):
    return DiffTestBlueprint(
        trace=Path('flow_events_json_v2.json'),
        query=Path('flow_events_test.sql'),
        out=Path('flow_events_json_v2.out'))

  def test_display_time_unit_slices(self):
    return DiffTestBlueprint(
        trace=Path('../../data/display_time_unit.json'),
        query=Path('slices_test.sql'),
        out=Path('display_time_unit_slices.out'))

  def test_sched_blocked_proto_sched_blocked_reason(self):
    return DiffTestBlueprint(
        trace=Path('sched_blocked_proto.py'),
        query=Path('sched_blocked_reason_test.sql'),
        out=Path('sched_blocked_proto_sched_blocked_reason.out'))

  def test_sched_blocked_systrace_sched_blocked_reason(self):
    return DiffTestBlueprint(
        trace=Path('sched_blocked_systrace.systrace'),
        query=Path('sched_blocked_reason_test.sql'),
        out=Path('sched_blocked_systrace_sched_blocked_reason.out'))

  def test_sched_blocked_reason_symbolized_sched_blocked_reason_function(self):
    return DiffTestBlueprint(
        trace=Path('sched_blocked_reason_symbolized.textproto'),
        query=Path('sched_blocked_reason_function_test.sql'),
        out=Path(
            'sched_blocked_reason_symbolized_sched_blocked_reason_function.out')
    )

  def test_sched_blocked_reason_symbolized_to_systrace(self):
    return DiffTestBlueprint(
        trace=Path('sched_blocked_reason_symbolized.textproto'),
        query=Path('../common/to_systrace_test.sql'),
        out=Path('sched_blocked_reason_symbolized_to_systrace.out'))

  def test_decimal_timestamp_slices(self):
    return DiffTestBlueprint(
        trace=Path('../../data/decimal_timestamp.json'),
        query=Path('slices_test.sql'),
        out=Path('decimal_timestamp_slices.out'))

  def test_counters_json_counters(self):
    return DiffTestBlueprint(
        trace=Path('../../data/counters.json'),
        query=Path('json_counters_test.sql'),
        out=Path('counters_json_counters.out'))

  def test_instants_json_instants(self):
    return DiffTestBlueprint(
        trace=Path('../../data/instants.json'),
        query=Path('json_instants_test.sql'),
        out=Path('instants_json_instants.out'))

  def test_very_long_sched_android_trace_quality(self):
    return DiffTestBlueprint(
        trace=Path('very_long_sched.py'),
        query=Metric('android_trace_quality'),
        out=Path('very_long_sched_android_trace_quality.out'))

  def test_sched_smoke_trailing_empty_2(self):
    return DiffTestBlueprint(
        trace=Path('../../data/atrace_b_193721088.atr'),
        query=Path('sched_smoke_test.sql'),
        out=Path('sched_smoke_trailing_empty.out'))

  def test_android_multiuser_switch(self):
    return DiffTestBlueprint(
        trace=Path('android_multiuser_switch.textproto'),
        query=Metric('android_multiuser'),
        out=Path('android_multiuser_switch.out'))

  def test_atrace_compressed_sched_count(self):
    return DiffTestBlueprint(
        trace=Path('../../data/atrace_compressed.ctrace'),
        query=Path('sched_smoke_test.sql'),
        out=Path('atrace_compressed_sched_count.out'))

  def test_atrace_uncompressed_sched_count(self):
    return DiffTestBlueprint(
        trace=Path('../../data/atrace_uncompressed_b_208691037'),
        query=Path('sched_smoke_test.sql'),
        out=Path('atrace_uncompressed_sched_count.out'))

  def test_otheruuids_android_other_traces(self):
    return DiffTestBlueprint(
        trace=Path('otheruuids.textproto'),
        query=Metric('android_other_traces'),
        out=Path('otheruuids_android_other_traces.out'))

  def test_android_binder(self):
    return DiffTestBlueprint(
        trace=Path('android_binder.py'),
        query=Metric('android_binder'),
        out=Path('android_binder.out'))

  def test_statsd_atoms_all_atoms(self):
    return DiffTestBlueprint(
        trace=Path('../../data/statsd_atoms.pb'),
        query=Path('all_atoms_test.sql'),
        out=Path('statsd_atoms_all_atoms.out'))

  def test_funcgraph_trace_funcgraph_test(self):
    return DiffTestBlueprint(
        trace=Path('funcgraph_trace.textproto'),
        query=Path('funcgraph_test.sql'),
        out=Path('funcgraph_trace_funcgraph_test.out'))
