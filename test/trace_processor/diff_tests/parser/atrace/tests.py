#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto, Systrace
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Atrace(TestSuite):
  # Match legacy Catapult behaviour when we see multiple S events b2b with the
  # cookie name and upid.
  def test_android_b2b_async_begin_list_slices(self):
    return DiffTestBlueprint(
        trace=Path('android_b2b_async_begin.textproto'),
        query="""
        SELECT ts, dur, name
        FROM slice;
        """,
        out=Csv("""
        "ts","dur","name"
        1000,30,"multistart"
        1015,45,"multistart"
        1030,20,"multistart"
        """))

  # Android userspace async slices
  def test_process_track_slices_android_async_slice(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 3
            event {
              timestamp: 74289018336
              pid: 4064
              print {
                ip: 18446743562018522420
                buf: "S|1204|launching: com.android.chrome|0\n"
              }
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 2
            event {
              timestamp: 74662603008
              pid: 1257
              print {
                ip: 18446743562018522420
                buf: "F|1204|launching: com.android.chrome|0\n"
              }
            }
          }
        }
        """),
        query="""
        SELECT
          ts,
          dur,
          pid,
          slice.name AS slice_name,
          process_track.name AS track_name
        FROM slice
        JOIN process_track ON slice.track_id = process_track.id
        JOIN process USING (upid);
        """,
        out=Path('process_track_slices_android_async_slice.out'))

  def test_async_track_atrace_process_track_slices(self):
    return DiffTestBlueprint(
        trace=Path('async_track_atrace.py'),
        query="""
        SELECT
          ts,
          dur,
          pid,
          slice.name AS slice_name,
          process_track.name AS track_name
        FROM slice
        JOIN process_track ON slice.track_id = process_track.id
        JOIN process USING (upid);
        """,
        out=Csv("""
        "ts","dur","pid","slice_name","track_name"
        50,25,1,"ev","track"
        55,15,1,"ev","track"
        60,5,2,"ev","track"
        """))

  # Resolving slice nesting issues when tracing both atrace and sys_write
  def test_sys_write_and_atrace(self):
    return DiffTestBlueprint(
        trace=Path('sys_write_and_atrace.py'),
        query="""
        SELECT slice.ts, slice.dur, slice.name, slice.depth
        FROM slice
        JOIN thread_track ON (slice.track_id = thread_track.id)
        JOIN thread USING (utid)
        WHERE tid = 42;
        """,
        out=Csv("""
        "ts","dur","name","depth"
        100,100,"sys_write",0
        300,50,"sys_write",0
        350,300,"test",0
        600,50,"sys_write",1
        """))

  def test_cpus_header_removal(self):
    return DiffTestBlueprint(
        trace=Systrace("""cpus=8
kworker/u16:1-77    (   77) [004] ....   316.196720: tracing_mark_write: B|77|test_slice
kworker/u16:1-77    (   77) [004] ....   316.196730: tracing_mark_write: E|77
        """),
        query="""
        SELECT ts, name, dur
        FROM slice
        WHERE name = 'test_slice';
        """,
        out=Csv("""
        "ts","name","dur"
        316196720000,"test_slice",10000
        """))

  def test_regex_pattern_no_gap_after_irq_flags(self):
    return DiffTestBlueprint(
        trace=Systrace("""cpus=8
kworker/u16:1-77    (   77) [004] ....316.196720: tracing_mark_write: B|77|no_gap_after_dots
kworker/u16:2-78    (   78) [005] .... 316.196730: tracing_mark_write: B|78|space_after_dots
test-process-123    (  123) [002] d...316.196740: tracing_mark_write: B|123|no_gap_after_flags
<idle>-0     [000]  0.002188: tracing_mark_write: E|77
<idle>-0     [001]  0.002198: tracing_mark_write: E|78
<idle>-0     [002]  0.002208: tracing_mark_write: E|123
        """),
        query="""
        SELECT name, COUNT(*) as count
        FROM slice
        WHERE name IN ('no_gap_after_dots', 'space_after_dots', 'no_gap_after_flags')
        GROUP BY name
        ORDER BY name;
        """,
        out=Csv("""
        "name","count"
        "no_gap_after_dots",1
        "no_gap_after_flags",1
        "space_after_dots",1
        """))

  def test_cpus_header_with_digits_only(self):
    return DiffTestBlueprint(
        trace=Systrace("""cpus=16
task-1234-567    ( 567) [000] ....   100.000000: tracing_mark_write: B|567|task_work
task-1234-567    ( 567) [000] ....   100.001000: tracing_mark_write: E|567
        """),
        query="""
        SELECT COUNT(*) as slice_count
        FROM slice
        WHERE name = 'task_work';
        """,
        out=Csv("""
        "slice_count"
        1
        """))
