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


class DiffTestModule_Tables(DiffTestModule):

  def test_android_sched_and_ps_smoke_window(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query=Path('smoke_window_test.sql'),
        out=Path('android_sched_and_ps_smoke_window.out'))

  def test_synth_1_filter_sched(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('filter_sched_test.sql'),
        out=Path('synth_1_filter_sched.out'))

  def test_android_sched_and_ps_b119496959(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query=Path('b119496959_test.sql'),
        out=Path('android_sched_and_ps_b119496959.out'))

  def test_android_sched_and_ps_b119301023(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query=Path('b119301023_test.sql'),
        out=Path('android_sched_and_ps_b119301023.out'))

  def test_synth_1_filter_counter(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('filter_counter_test.sql'),
        out=Path('synth_1_filter_counter.out'))

  def test_memory_counters_b120278869_neg_ts_end(self):
    return DiffTestBlueprint(
        trace=Path('../../data/memory_counters.pb'),
        query=Path('b120278869_neg_ts_end_test.sql'),
        out=Path('memory_counters_b120278869_neg_ts_end.out'))

  def test_counters_where_cpu_counters_where_cpu(self):
    return DiffTestBlueprint(
        trace=Path('counters_where_cpu.py'),
        query=Path('counters_where_cpu_test.sql'),
        out=Path('counters_where_cpu_counters_where_cpu.out'))

  def test_counters_group_by_freq_counters_group_by_freq(self):
    return DiffTestBlueprint(
        trace=Path('counters_group_by_freq.py'),
        query=Path('counters_group_by_freq_test.sql'),
        out=Path('counters_group_by_freq_counters_group_by_freq.out'))

  def test_filter_row_vector_example_android_trace_30s(self):
    return DiffTestBlueprint(
        trace=Path('../../data/example_android_trace_30s.pb'),
        query=Path('filter_row_vector_test.sql'),
        out=Path('filter_row_vector_example_android_trace_30s.out'))

  def test_counter_dur_example_android_trace_30s(self):
    return DiffTestBlueprint(
        trace=Path('../../data/example_android_trace_30s.pb'),
        query=Path('counter_dur_test.sql'),
        out=Path('counter_dur_example_android_trace_30s.out'))

  def test_nulls(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('nulls_test.sql'),
        out=Path('nulls.out'))

  def test_thread_main_thread(self):
    return DiffTestBlueprint(
        trace=Path('thread_main_thread.textproto'),
        query=Path('thread_main_thread_test.sql'),
        out=Path('thread_main_thread.out'))

  def test_trace_metadata(self):
    return DiffTestBlueprint(
        trace=Path('../../data/memory_counters.pb'),
        query=Path('trace_metadata'),
        out=Path('trace_metadata.json.out'))

  def test_android_task_names(self):
    return DiffTestBlueprint(
        trace=Path('process_uids.textproto'),
        query=Path('android_task_names'),
        out=Path('android_task_names.out'))

  def test_ftrace_setup_errors(self):
    return DiffTestBlueprint(
        trace=Path('../../data/ftrace_error_stats.pftrace'),
        query=Path('ftrace_setup_errors_test.sql'),
        out=Path('ftrace_setup_errors.out'))
