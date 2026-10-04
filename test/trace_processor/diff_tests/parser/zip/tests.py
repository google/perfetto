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

from python.generators.diff_tests.testing import Csv, Path, DataPath
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from python.generators.diff_tests.testing import TextProto
from python.generators.diff_tests.testing import Tar
from python.generators.diff_tests.testing import Zip as ZipTrace


def _ftrace(*events, cpu=0, first_packet=False):
  pfx = 'first_packet_on_sequence: true ' if first_packet else ''
  return f'packet {{ {pfx}ftrace_events {{ cpu: {cpu} {" ".join(events)} }} }}'


def _cpu_freq(ts, state, cpu=0, pid=1):
  return (f'event {{ timestamp: {ts} pid: {pid} cpu_frequency {{ '
          f'state: {state} cpu_id: {cpu} }} }}')


def _two_trace_zip(trace1, trace2=None):
  """ZIP with trace1.pftrace and trace2.pftrace (a copy of trace1 if None)."""
  return ZipTrace({
      'trace1.pftrace': TextProto(trace1),
      'trace2.pftrace': TextProto(trace1 if trace2 is None else trace2),
  })


class Zip(TestSuite):

  def test_perf_proto_sym(self):
    return DiffTestBlueprint(
        trace=DataPath('zip/perf_track_sym.zip'),
        query=Path('../simpleperf/stacks_test.sql'),
        out=Csv('''
        "name"
        "main,A"
        "main,A,B"
        "main,A,B,C"
        "main,A,B,C,D"
        "main,A,B,C,D,E"
        "main,A,B,C,E"
        "main,A,B,D"
        "main,A,B,D,E"
        "main,A,B,E"
        "main,A,C"
        "main,A,C,D"
        "main,A,C,D,E"
        "main,A,C,E"
        "main,A,D"
        "main,A,D,E"
        "main,A,E"
        "main,B"
        "main,B,C"
        "main,B,C,D"
        "main,B,C,D,E"
        "main,B,C,E"
        "main,B,D"
        "main,B,D,E"
        "main,B,E"
        "main,C"
        "main,C,D"
        "main,C,D,E"
        "main,C,E"
        "main,D"
        "main,D,E"
        "main,E"
        '''))

  def test_zip_tokenization_order(self):
    return DiffTestBlueprint(
        trace=DataPath('zip/perf_track_sym.zip'),
        query='''
          SELECT id, parent_id, name, size, trace_type, processing_order
          FROM __intrinsic_trace_file
          ORDER BY processing_order
        ''',
        out=Csv('''
        "id","parent_id","name","size","trace_type","processing_order"
        0,"[NULL]","[NULL]",94651,"zip",0
        3,0,"c.trace.pb",379760,"proto",1
        1,0,"b.simpleperf.data",554911,"perf",2
        2,0,"a.symbols.pb",186149,"symbols",3
        '''))

  def test_tar_gz_tokenization_order(self):
    return DiffTestBlueprint(
        trace=DataPath('perf_track_sym.tar.gz'),
        query='''
          SELECT id, parent_id, name, size, trace_type, processing_order
          FROM __intrinsic_trace_file
          ORDER BY processing_order
        ''',
        out=Csv('''
        "id","parent_id","name","size","trace_type","processing_order"
        0,"[NULL]","[NULL]",94091,"gzip",0
        1,0,"",1126400,"tar",1
        4,1,"c.trace.pb",379760,"proto",2
        3,1,"b.simpleperf.data",554911,"perf",3
        2,1,"a.symbols.pb",186149,"symbols",4
        '''))

  # Make sure the logcat timestamps are correctly converted to trace ts. All
  # logcat events in the trace were emitted while a perfetto trace collection
  # was active. Thus their timestamps should be between the min and max ts of
  # all track events.
  # The device where the trace was collected had a timezone setting of UTC+1
  def test_logcat_and_proto(self):
    return DiffTestBlueprint(
        trace=DataPath('zip/logcat_and_proto.zip'),
        query='''
        WITH
          INTERVAL AS (
            SELECT
              (SELECT MIN(ts) FROM slice) AS min_ts,
              (SELECT MAX(ts) FROM slice) AS max_ts
          )
        SELECT COUNT(*) AS count
        FROM android_logs, INTERVAL
        WHERE ts BETWEEN min_ts AND max_ts;
        ''',
        out=Csv('''
        "count"
        58
        '''))

  # A zip assembled inline from blueprint members: a textproto-defined proto
  # trace and a raw-text systrace. Proto members are processed first.
  def test_zip_blueprint_inline_members(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            'a.systrace':
                '''# tracer: nop
#
  app-100 (  100) [001] ...1  1.000000: tracing_mark_write: B|100|zip_slice
  app-100 (  100) [001] ...1  1.500000: tracing_mark_write: E|100
''',
            'b.pb':
                TextProto('''
              packet {
                timestamp: 1
                process_tree {
                  processes { pid: 5 ppid: 0 cmdline: "proc_in_zip" }
                }
              }
            '''),
        }),
        query='''
          SELECT name, trace_type, processing_order
          FROM __intrinsic_trace_file
          ORDER BY processing_order;
        ''',
        out=Csv('''
        "name","trace_type","processing_order"
        "[NULL]","zip",0
        "b.pb","proto",1
        "a.systrace","systrace",2
        '''))

  # Systrace timestamps must be written back after clock conversion: inside a
  # zip with a proto trace providing a MONOTONIC<->BOOTTIME snapshot the
  # conversion is not the identity, and the value written to tables must be
  # the converted one (which is also the sorting key). The 1.0s MONOTONIC
  # slice lands at BOOTTIME 1_000_000_000 + 500_000_000.
  def test_zip_systrace_converted_timestamps(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            'sys.systrace':
                '''# tracer: nop
#
  app-100 (  100) [001] ...1  1.000000: tracing_mark_write: B|100|sys_slice
  app-100 (  100) [001] ...1  1.500000: tracing_mark_write: E|100
''',
            'spine.pb':
                TextProto('''
              packet {
                clock_snapshot {
                  clocks { clock_id: 6 timestamp: 1000000000 }
                  clocks { clock_id: 3 timestamp: 500000000 }
                }
              }
            '''),
        }),
        query='''
          SELECT name, ts, dur FROM slice WHERE name = 'sys_slice';
        ''',
        out=Csv('''
        "name","ts","dur"
        "sys_slice",1500000000,500000000
        '''))

  # A tar archive with an external file (DataPath) as a member: the raw bytes
  # of the checked-in trace are included verbatim.
  def test_tar_blueprint_external_member(self):
    return DiffTestBlueprint(
        trace=Tar({
            'sched.pb':
                DataPath('synth_1.pb'),
            'log.systrace':
                '''# tracer: nop
#
  app-100 (  100) [001] ...1  1.000000: tracing_mark_write: B|100|tar_slice
  app-100 (  100) [001] ...1  1.500000: tracing_mark_write: E|100
''',
        }),
        query='''
          SELECT
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT count(*) FROM __intrinsic_trace_file
             WHERE name = 'sched.pb') AS named_member;
        ''',
        out=Csv('''
        "sched_count","named_member"
        8,1
        '''))

  # A tar archive as produced by macOS/BSD `tar`: PAX extended headers,
  # space-terminated numeric fields, and AppleDouble ("._foo") / ".DS_Store"
  # metadata files sprinkled next to the real trace. The real trace must load
  # and every hidden (dot-prefixed) entry must be ignored, including one nested
  # under a subdirectory.
  def test_macos_tar(self):
    return DiffTestBlueprint(
        trace=Tar(
            {
                'sched.pb': DataPath('synth_1.pb'),
                '._sched.pb': b'Mac OS X AppleDouble junk\x00\x01\x02',
                '.DS_Store': b'Bud1\x00\x00\x00\x00junk',
                'sub/._nested.pb': b'more junk',
            },
            macos_style=True,
        ),
        query='''
          SELECT
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT count(*) FROM __intrinsic_trace_file) AS file_count,
            (SELECT count(*) FROM __intrinsic_trace_file
             WHERE name GLOB '*._*' OR name GLOB '*.DS_Store') AS hidden_count;
        ''',
        out=Csv('''
        "sched_count","file_count","hidden_count"
        8,2,0
        '''))

  def test_multi_trace_single_machine_clock(self):
    return DiffTestBlueprint(
        trace=DataPath('multi_trace_single_machine_clock.zip'),
        query='''
        SELECT ts
        FROM slice
        WHERE name = 'InterruptibleSleep::run_with_interval';
        ''',
        out=Csv('''
        "ts"
        1276407306585477
        1276408471040116
        '''))

  # Two traces from one machine share one cpu_frequency track per CPU and keep
  # both traces' samples.
  def test_two_traces_share_cpu_frequency_track(self):
    return DiffTestBlueprint(
        trace=_two_trace_zip(
            _ftrace(_cpu_freq(1000, 1000000), _cpu_freq(1001, 1200000)),
            _ftrace(_cpu_freq(1000, 1000000), _cpu_freq(1002, 1400000))),
        query='''
          SELECT
            (SELECT count(*) FROM cpu_counter_track WHERE type = 'cpu_frequency')
              AS cpufreq_tracks,
            t.name,
            t.cpu,
            c.ts,
            c.value
          FROM counter c
          JOIN cpu_counter_track t ON c.track_id = t.id
          WHERE t.type = 'cpu_frequency'
          ORDER BY c.ts, c.value;
        ''',
        out=Csv('''
        "cpufreq_tracks","name","cpu","ts","value"
        1,"cpufreq",0,1000,1000000.000000
        1,"cpufreq",0,1000,1000000.000000
        1,"cpufreq",0,1001,1200000.000000
        1,"cpufreq",0,1002,1400000.000000
        '''))
