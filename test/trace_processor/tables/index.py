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


class DiffTestModule_Tables(DiffTestModule):

  def test_android_sched_and_ps_smoke_window(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query=Path('smoke_window_test.sql'),
        out=Csv("""
"ts","dur","quantum_ts"
0,9223372036854775807,0
"""))

  def test_synth_1_filter_sched(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('filter_sched_test.sql'),
        out=Csv("""
"ts","cpu","dur"
170,1,80
"""))

  def test_android_sched_and_ps_b119496959(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query=Path('b119496959_test.sql'),
        out=Csv("""
"ts","cpu"
81473797824982,3
81473797942847,3
81473798135399,0
81473798786857,2
81473798875451,3
81473799019930,2
81473799079982,0
81473800089357,3
81473800144461,3
81473800441805,3
"""))

  def test_android_sched_and_ps_b119301023(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query=Path('b119301023_test.sql'),
        out=Csv("""
"ts"
81473010031230
81473010109251
81473010121751
81473010179772
81473010203886
81473010234720
81473010278522
81473010308470
81473010341386
81473010352792
"""))

  def test_synth_1_filter_counter(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('filter_counter_test.sql'),
        out=Csv("""
"COUNT(*)"
2
"""))

  def test_memory_counters_b120278869_neg_ts_end(self):
    return DiffTestBlueprint(
        trace=Path('../../data/memory_counters.pb'),
        query=Path('b120278869_neg_ts_end_test.sql'),
        out=Csv("""
"count(*)"
98688
"""))

  def test_counters_where_cpu_counters_where_cpu(self):
    return DiffTestBlueprint(
        trace=Path('counters_where_cpu.py'),
        query=Path('counters_where_cpu_test.sql'),
        out=Csv("""
"ts","dur","value"
1000,1,3000.000000
1001,0,4000.000000
"""))

  def test_counters_group_by_freq_counters_group_by_freq(self):
    return DiffTestBlueprint(
        trace=Path('counters_group_by_freq.py'),
        query=Path('counters_group_by_freq_test.sql'),
        out=Csv("""
"value","dur_sum"
4000.000000,2
3000.000000,1
"""))

  def test_filter_row_vector_example_android_trace_30s(self):
    return DiffTestBlueprint(
        trace=Path('../../data/example_android_trace_30s.pb'),
        query=Path('filter_row_vector_test.sql'),
        out=Path('filter_row_vector_example_android_trace_30s.out'))

  def test_counter_dur_example_android_trace_30s(self):
    return DiffTestBlueprint(
        trace=Path('../../data/example_android_trace_30s.pb'),
        query=Path('counter_dur_test.sql'),
        out=Csv("""
"ts","dur"
100351738640,-1
100351738640,-1
100351738640,-1
70731059648,19510835
70731059648,19510835
70731059648,19510835
73727335051,23522762
73727335051,23522762
73727335051,23522762
86726132752,24487554
"""))

  def test_nulls(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('nulls_test.sql'),
        out=Path('nulls.out'))

  def test_thread_main_thread(self):
    return DiffTestBlueprint(
        trace=Path('thread_main_thread.textproto'),
        query=Path('thread_main_thread_test.sql'),
        out=Csv("""
"tid","is_main_thread"
5,1
7,0
11,1
12,0
99,"[NULL]"
"""))

  def test_trace_metadata(self):
    return DiffTestBlueprint(
        trace=Path('../../data/memory_counters.pb'),
        query=Metric('trace_metadata'),
        out=Path('trace_metadata.json.out'))

  def test_android_task_names(self):
    return DiffTestBlueprint(
        trace=Path('process_uids.textproto'),
        query=Metric('android_task_names'),
        out=TextProto(r"""
android_task_names {
  process {
    pid: 1
    process_name: "init"
    uid: 0
  }
  process {
    pid: 2
    process_name: "com.google.android.gm:process"
    uid: 10001
    uid_package_name: "com.google.android.gm"
  }
}
"""))

  def test_ftrace_setup_errors(self):
    return DiffTestBlueprint(
        trace=Path('../../data/ftrace_error_stats.pftrace'),
        query=Path('ftrace_setup_errors_test.sql'),
        out=Csv("""
"value"
3
"Ftrace event unknown: foo/bar
Ftrace event unknown: sched/foobar
Atrace failures: error: unknown tracing category "bar"
error enabling tracing category "bar"
"
"""))
