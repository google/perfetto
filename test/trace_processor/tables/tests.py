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
from python.generators.diff_tests.testing import TestSuite


class Tables(TestSuite):

  def test_android_sched_and_ps_smoke_window(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query="""
        SELECT * FROM "window";
        """,
        out=Csv("""
        "ts","dur","quantum_ts"
        0,9223372036854775807,0
        """))

  def test_nulls(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query="""
        CREATE TABLE null_test (
          primary_key INTEGER PRIMARY KEY,
          int_nulls INTEGER,
          string_nulls STRING,
          double_nulls DOUBLE,
          start_int_nulls INTEGER,
          start_string_nulls STRING,
          start_double_nulls DOUBLE,
          all_nulls INTEGER
        );
        
        INSERT INTO null_test(
          int_nulls,
          string_nulls,
          double_nulls,
          start_int_nulls,
          start_string_nulls,
          start_double_nulls
        )
        VALUES
        (1, "test", 2.0, NULL, NULL, NULL),
        (2, NULL, NULL, NULL, "test", NULL),
        (1, "other", NULL, NULL, NULL, NULL),
        (4, NULL, NULL, NULL, NULL, 1.0),
        (NULL, "test", 1.0, 1, NULL, NULL);
        
        SELECT * FROM null_test;
        """,
        out=Path('nulls.out'))

  def test_thread_main_thread(self):
    return DiffTestBlueprint(
        trace=Path('thread_main_thread.textproto'),
        query="""
        SELECT
          tid,
          is_main_thread
        FROM thread
        WHERE tid IN (5, 7, 11, 12, 99)
        ORDER BY tid;
        """,
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
        query="""
        SELECT value FROM stats WHERE name = 'ftrace_setup_errors'
        UNION ALL
        SELECT str_value FROM metadata WHERE name = 'ftrace_setup_errors';
        """,
        out=Csv("""
        "value"
        3
        "Ftrace event unknown: foo/bar
        Ftrace event unknown: sched/foobar
        Atrace failures: error: unknown tracing category "bar"
        error enabling tracing category "bar"
        "
        """))
