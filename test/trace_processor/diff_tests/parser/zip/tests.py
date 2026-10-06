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


def _sched_switch(ts, pcomm, ppid, ncomm, npid, prev_state=0, prio=120):
  return (f'event {{ timestamp: {ts} pid: {ppid} sched_switch {{ '
          f'prev_comm: "{pcomm}" prev_pid: {ppid} prev_state: {prev_state} '
          f'next_comm: "{ncomm}" next_pid: {npid} next_prio: {prio} }} }}')


def _sched_waking(ts, pid, comm, target_pid, prio=120, target_cpu=0):
  return (f'event {{ timestamp: {ts} pid: {pid} sched_waking {{ '
          f'comm: "{comm}" pid: {target_pid} prio: {prio} '
          f'target_cpu: {target_cpu} }} }}')


def _task_newtask(ts, pid, child_pid, comm, clone_flags=0):
  return (f'event {{ timestamp: {ts} pid: {pid} task_newtask {{ '
          f'pid: {child_pid} comm: "{comm}" clone_flags: {clone_flags} }} }}')


def _compact_sched(intern_table, switches):
  fields = [f'intern_table: "{t}"' for t in intern_table]
  for attr, idx in [
      ('switch_timestamp', 0),
      ('switch_next_pid', 1),
      ('switch_prev_state', 2),
      ('switch_next_prio', 3),
      ('switch_next_comm_index', 4),
  ]:
    fields.extend(f'{attr}: {s[idx]}' for s in switches)
  return f'compact_sched {{ {" ".join(fields)} }}'


def _two_trace_zip(trace1,
                   trace2=None,
                   name1='trace1.pftrace',
                   name2='trace2.pftrace'):
  """ZIP with trace1.pftrace and trace2.pftrace (a copy of trace1 if None)."""
  return ZipTrace({
      name1: TextProto(trace1),
      name2: TextProto(trace1 if trace2 is None else trace2),
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

  # In C, trace 1 owns until ts 1001; trace 2's sample at 1000 is dropped and
  # cached (1 conflict). At 1002 (> 1001), handover emits the cached value and
  # keeps trace 2's sample (total 4 rows).
  def test_two_traces_duplicate_cpu_frequency(self):
    return DiffTestBlueprint(
        trace=_two_trace_zip(
            _ftrace(_cpu_freq(1000, 1000000), _cpu_freq(1001, 1200000)),
            _ftrace(_cpu_freq(1000, 1000000), _cpu_freq(1002, 1400000))),
        query='''
          SELECT
            (SELECT count(*) FROM counter) AS counter_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_counter_claim_conflict') AS conflict_count;
        ''',
        out=Csv('''
        "counter_count","conflict_count"
        3,1
        '''))

  # Duplicate sched_switch events across traces report claim conflicts.
  def test_two_traces_duplicate_sched(self):
    return DiffTestBlueprint(
        trace=_two_trace_zip(
            _ftrace(
                _sched_switch(1000, 't1', 100, 't2', 200),
                _sched_switch(2000, 't2', 200, 't1', 100),
                _sched_switch(3000, 't1', 100, 't2', 200)),
            _ftrace(
                _sched_switch(1500, 't3', 300, 't4', 400),
                _sched_switch(2500, 't4', 400, 't3', 300))),
        query='''
          SELECT
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS conflict_count;
        ''',
        out=Csv('''
        "sched_count","conflict_count"
        3,2
        '''))

  # Independent claims across traces have no claim conflicts.
  def test_two_traces_independent_claims(self):
    return DiffTestBlueprint(
        trace=_two_trace_zip(
            _ftrace(_cpu_freq(1000, 1000000), _cpu_freq(1001, 1200000)),
            _ftrace(
                _sched_switch(1000, 't1', 100, 't2', 200),
                _sched_switch(2000, 't2', 200, 't1', 100),
                _sched_switch(3000, 't1', 100, 't2', 200))),
        query='''
          SELECT
            (SELECT count(*) FROM counter) AS counter_count,
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_counter_claim_conflict') AS counter_conflicts,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS sched_conflicts;
        ''',
        out=Csv('''
        "counter_count","sched_count","counter_conflicts","sched_conflicts"
        2,3,0,0
        '''))

  # In C, b.pb owns [50, 60]. At 100 (> 60), a.pb takes over via handover.
  def test_sched_claim_earlier_timestamp_wins(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            'b.pb':
                TextProto(
                    _ftrace(
                        _sched_switch(50, 't1', 10, 't2', 20),
                        _sched_switch(60, 't2', 20, 't1', 10))),
            'a.pb':
                TextProto(
                    _ftrace(
                        _sched_switch(100, 't3', 30, 't4', 40),
                        _sched_switch(120, 't4', 40, 't3', 30))),
        }),
        query='''
          SELECT
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT max(ts) FROM sched) AS max_sched_ts,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS conflict_count;
        ''',
        out=Csv('''
        "sched_count","max_sched_ts","conflict_count"
        4,120,0
        '''))

  # Compact sched bundles in duplicate traces deduplicate cleanly. All 3 of
  # trace 2's compact switches count as dropped, including the first one per
  # CPU (which never makes a slice but still claims / conflicts).
  def test_two_traces_compact_sched(self):
    compact_payload = _ftrace(
        _compact_sched(['t1', 't2'], [
            (1000, 10, 0, 120, 0),
            (1000, 20, 0, 120, 1),
            (1000, 10, 0, 120, 0),
        ]))
    return DiffTestBlueprint(
        trace=_two_trace_zip(compact_payload),
        query='''
          SELECT
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS conflict_count;
        ''',
        out=Csv('''
        "sched_count","conflict_count"
        2,3
        '''))

  # task_newtask does not claim scheduling; later switch claims it.
  def test_newtask_does_not_claim_sched(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            'a.pb':
                TextProto(
                    _ftrace(
                        _task_newtask(500, 100, 200, 'forked_child'),
                        _task_newtask(600, 100, 201, 'forked_child2'),
                        first_packet=True)),
            'b.pb':
                TextProto(
                    _ftrace(
                        _sched_switch(1000, 't1', 10, 't2', 20),
                        _sched_switch(2000, 't2', 20, 't1', 10))),
        }),
        query='''
          SELECT
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS sched_conflicts;
        ''',
        out=Csv('''
        "sched_count","sched_conflicts"
        2,0
        '''))

  # Single trace with frequency and sched switch has no claim conflicts.
  def test_single_trace_no_claim_conflicts(self):
    return DiffTestBlueprint(
        trace=TextProto(
            _ftrace(
                _cpu_freq(1000, 1000000), _sched_switch(1000, 't1', 10, 't2',
                                                        20),
                _sched_switch(2000, 't2', 20, 't1', 10))),
        query='''
          SELECT
            (SELECT count(*) FROM counter) AS counter_count,
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_counter_claim_conflict') AS counter_conflicts,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS sched_conflicts;
        ''',
        out=Csv('''
        "counter_count","sched_count","counter_conflicts","sched_conflicts"
        1,2,0,0
        '''))

  # Two identical traces with switch + waking + switch. Trace 1 owns sched
  # from its first sched_switch, so all three of trace 2's events (two
  # switches and the waking) are dropped and thread_state has the same 5
  # rows as a single trace. Note: wakings that arrive before the owner's
  # first sched_switch are kept (nobody owns sched yet), which is why the
  # waking comes second.
  def test_two_traces_sched_waking(self):
    waking_payload = _ftrace(
        _sched_switch(1000, 't1', 10, 't2', 20, prev_state=1),
        _sched_waking(1500, 20, 't1', 10),
        _sched_switch(2500, 't2', 20, 't1', 10))
    return DiffTestBlueprint(
        trace=_two_trace_zip(waking_payload),
        query='''
          SELECT
            (SELECT count(*) FROM thread_state) AS thread_state_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS conflict_count;
        ''',
        out=Csv('''
        "thread_state_count","conflict_count"
        5,3
        '''))

  # Two identical traces where a thread is created mid-trace. Trace 2's
  # task_newtask must be dropped as a whole: if it still started a thread,
  # tid 30 would get a second utid and the owner's Runnable state would be
  # left open on the first one. Expect one thread with 3 states (R, Running,
  # R), like a single trace, and 4 dropped events from trace 2.
  def test_two_traces_task_newtask_no_split_thread(self):
    payload = _ftrace(
        _sched_switch(500, 'p', 10, 'q', 20),
        _task_newtask(1000, 10, 30, 'child', clone_flags=65536),
        _sched_switch(1500, 'q', 20, 'child', 30),
        _sched_switch(2500, 'child', 30, 'q', 20))
    return DiffTestBlueprint(
        trace=_two_trace_zip(payload),
        query='''
          SELECT
            (SELECT count(*) FROM thread WHERE tid = 30) AS threads,
            (SELECT count(*) FROM thread_state JOIN thread USING (utid)
             WHERE tid = 30) AS states,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS conflict_count;
        ''',
        out=Csv('''
        "threads","states","conflict_count"
        1,3,4
        '''))

  # Same as above but the thread is created before either trace has a
  # sched_switch, so nobody owns scheduling yet. The duplicate task_newtask
  # must still reuse the existing thread instead of splitting it.
  def test_two_traces_task_newtask_before_claim_no_split_thread(self):
    payload = _ftrace(
        _task_newtask(500, 10, 30, 'child', clone_flags=65536),
        _sched_switch(1000, 'p', 10, 'child', 30),
        _sched_switch(2000, 'child', 30, 'p', 10))
    return DiffTestBlueprint(
        trace=_two_trace_zip(payload),
        query='''
          SELECT
            (SELECT count(*) FROM thread WHERE tid = 30) AS threads,
            (SELECT count(*) FROM sched JOIN thread USING (utid)
             WHERE tid = 30) AS sched_rows;
        ''',
        out=Csv('''
        "threads","sched_rows"
        1,1
        '''))

  # Back-to-back traces: in C, trace 2 starts after trace 1 ends, so trace 2's
  # window is non-overlapping. Trace 2's sched data is kept (0 conflicts).
  def test_back_to_back_fork_keeps_process(self):
    return DiffTestBlueprint(
        trace=_two_trace_zip(
            _ftrace(
                _sched_switch(100, 'a', 10, 'b', 20),
                _sched_switch(200, 'b', 20, 'a', 10)),
            _ftrace(
                _sched_switch(1000, 'a', 10, 'b', 20),
                _task_newtask(1100, 10, 40, 'forked'))),
        query='''
          SELECT
            (SELECT count(*) FROM thread WHERE tid = 40) AS threads,
            (SELECT count(*) FROM process WHERE pid = 40) AS processes,
            (SELECT count(*) FROM thread_state JOIN thread USING (utid)
             WHERE tid = 40) AS states,
            (SELECT group_concat(f.name) FROM stats s
             JOIN __intrinsic_trace_file f ON s.trace_id = f.id
             WHERE s.name = 'machine_sched_claim_conflict' AND s.value > 0)
              AS conflict_trace,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS conflict_count;
        ''',
        out=Csv('''
        "threads","processes","states","conflict_trace","conflict_count"
        1,1,1,"[NULL]",0
        '''))

  # The first compact switch on a CPU claims scheduling too, so trace 2's
  # waking right after it is dropped instead of leaving a second open
  # Runnable state on the same thread.
  def test_two_traces_compact_first_switch_claims(self):
    payload = _ftrace(
        _compact_sched(['t1'], [(1000, 10, 0, 120, 0)]),
        _sched_waking(1500, 30, 't2', 20))
    return DiffTestBlueprint(
        trace=_two_trace_zip(payload),
        query='''
          SELECT
            (SELECT count(*) FROM thread_state JOIN thread USING (utid)
             WHERE tid = 20) AS states,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS conflict_count;
        ''',
        out=Csv('''
        "states","conflict_count"
        1,2
        '''))

  # Duplicate generic kernel task state events: trace 2 is dropped before it
  # can start or end threads, so there is one thread and one slice.
  def test_two_traces_generic_kernel(self):
    payload = '''
        packet {
          timestamp: 1000
          generic_kernel_task_state_event {
            cpu: 0 comm: "task1" tid: 101 state: 3 prio: 100
          }
        }
        packet {
          timestamp: 2000
          generic_kernel_task_state_event {
            cpu: 0 comm: "task1" tid: 101 state: 8 prio: 100
          }
        }
        '''
    return DiffTestBlueprint(
        trace=_two_trace_zip(payload),
        query='''
          SELECT
            (SELECT count(*) FROM sched_slice) AS slices,
            (SELECT count(*) FROM thread WHERE tid = 101) AS threads,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS conflict_count;
        ''',
        out=Csv('''
        "slices","threads","conflict_count"
        1,1,2
        '''))

  # Two traces from one machine share one cpu_frequency track per CPU; trace 1
  # claims ownership on [1000, 1001], so trace 2's sample at 1000 is dropped and
  # cached. At 1002 (> 1001), handover writes the cached value and keeps trace 2's
  # sample at 1002.
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
        1,"cpufreq",0,1001,1200000.000000
        1,"cpufreq",0,1002,1400000.000000
        '''))

  # Trace 1 provides battery capacity only and trace 2 provides current only.
  # These are different tracks, so both are kept with no conflicts.
  def test_two_traces_battery_counter_subsets(self):
    t1 = """
        packet { timestamp: 1000 battery { capacity_percent: 80 } }
        packet { timestamp: 3000 battery { capacity_percent: 79 } }
        packet { timestamp: 5000 battery { capacity_percent: 78 } }
        packet { timestamp: 7000 battery { capacity_percent: 77 } }
        packet { timestamp: 9000 battery { capacity_percent: 76 } }
        """
    t2 = """
        packet { timestamp: 2000 battery { current_ua: -200000 } }
        packet { timestamp: 4000 battery { current_ua: -210000 } }
        packet { timestamp: 6000 battery { current_ua: -190000 } }
        packet { timestamp: 8000 battery { current_ua: -220000 } }
        packet { timestamp: 10000 battery { current_ua: -205000 } }
        """
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query='''
          SELECT
            t.name,
            count(c.id) AS count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_counter_claim_conflict') AS conflict_count
          FROM counter_track t
          JOIN counter c ON c.track_id = t.id
          WHERE t.name LIKE 'batt.%'
          GROUP BY t.name
          ORDER BY t.name;
        ''',
        out=Csv('''
        "name","count","conflict_count"
        "batt.capacity_pct",5,0
        "batt.current_ua",5,0
        '''))

  # When trace 2 has an early waking before any trace claims sched, it opens
  # a runnable state. When trace 1's first context switch claims scheduling,
  # trace 2's open state is closed at that switch timestamp so that no
  # thread states overlap.
  def test_early_waking_closed_at_first_sched_claim(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            't1.pb':
                TextProto(
                    _ftrace(
                        _sched_switch(2000, 't1', 10, 't2', 20, prev_state=1),
                        _sched_switch(3000, 't2', 20, 't1', 10, prev_state=1),
                        _sched_switch(4000, 't1', 10, 't2', 20, prev_state=1),
                    )),
            't2.pb':
                TextProto(
                    _ftrace(
                        _sched_waking(1000, 1, 't2', 20),
                        _sched_switch(2500, 't3', 30, 't2', 20, prev_state=1),
                        _sched_switch(3500, 't2', 20, 't3', 30, prev_state=1),
                    )),
        }),
        query='''
          SELECT
            (SELECT count(*) FROM thread_state a
             JOIN thread_state b ON a.utid = b.utid AND b.ts > a.ts
             AND (a.dur = -1 OR a.ts + a.dur > b.ts)) AS overlap_count,
            (SELECT tid FROM thread t WHERE t.utid = ts.utid) AS tid,
            ts.ts,
            ts.dur,
            ts.state
          FROM thread_state ts
          ORDER BY tid, ts.ts;
        ''',
        out=Csv('''
        "overlap_count","tid","ts","dur","state"
        0,10,2000,1000,"S"
        0,10,3000,1000,"Running"
        0,10,4000,-1,"S"
        0,20,1000,1000,"R"
        0,20,2000,1000,"Running"
        0,20,3000,1000,"S"
        0,20,4000,-1,"Running"
        '''))

  # JSON thread state ('T') events are guarded by KeepSched; once trace 1
  # provides sched, duplicate thread states from trace 2 are dropped.
  def test_two_traces_json_thread_state(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            't1.json':
                '[{"name": "Running", "ph": "T", "ts": 1, "dur": 1, "pid": 1, "tid": 1, "args": {"cpu": 0}}]',
            't2.json':
                '[{"name": "Running", "ph": "T", "ts": 2, "dur": 1, "pid": 1, "tid": 1, "args": {"cpu": 0}}]',
        }),
        query='''
          SELECT
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS conflict_count;
        ''',
        out=Csv('''
        "sched_count","conflict_count"
        1,1
        '''))

  # Merging identical traces gives identical sched/counter rows.
  def test_lease_identical_traces(self):
    payload = _ftrace(
        _sched_switch(1000, 't1', 10, 't2', 20),
        _sched_switch(2000, 't2', 20, 't1', 10), _cpu_freq(1000, 1000000),
        _cpu_freq(2000, 1200000))
    return DiffTestBlueprint(
        trace=_two_trace_zip(payload),
        query='''
          SELECT
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT count(*) FROM counter) AS counter_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS sched_conflicts,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_counter_claim_conflict') AS counter_conflicts;
        ''',
        out=Csv('''
        "sched_count","counter_count","sched_conflicts","counter_conflicts"
        2,2,2,2
        '''))

  # Partial overlap: later trace's data after the first ends is kept via handover.
  def test_lease_partial_overlap(self):
    return DiffTestBlueprint(
        trace=_two_trace_zip(
            _ftrace(
                _sched_switch(1000, 't1', 10, 't2', 20),
                _sched_switch(2000, 't2', 20, 't1', 10)),
            _ftrace(
                _sched_switch(1500, 't3', 30, 't4', 40),
                _sched_switch(3000, 't4', 40, 't3', 30))),
        query='''
          SELECT
            ts,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS conflict_count
          FROM sched
          ORDER BY ts;
        ''',
        out=Csv('''
        "ts","conflict_count"
        1000,1
        2000,1
        3000,1
        '''))

  # Enclosed: long trace starting first keeps all, short trace dropped.
  def test_lease_enclosed(self):
    return DiffTestBlueprint(
        trace=_two_trace_zip(
            _ftrace(
                _sched_switch(1000, 't1', 10, 't2', 20),
                _sched_switch(2000, 't2', 20, 't1', 10),
                _sched_switch(4000, 't1', 10, 't2', 20)),
            _ftrace(
                _sched_switch(1500, 't3', 30, 't4', 40),
                _sched_switch(2500, 't4', 40, 't3', 30))),
        query='''
          SELECT
            ts,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS conflict_count
          FROM sched
          ORDER BY ts;
        ''',
        out=Csv('''
        "ts","conflict_count"
        1000,2
        2000,2
        4000,2
        '''))

  # Enclosed reversed: short trace starts first; short owns its span, long
  # takes over after short ends.
  def test_lease_enclosed_reversed(self):
    return DiffTestBlueprint(
        trace=_two_trace_zip(
            _ftrace(
                _sched_switch(1000, 't1', 10, 't2', 20),
                _sched_switch(2000, 't2', 20, 't1', 10)),
            _ftrace(
                _sched_switch(1500, 't3', 30, 't4', 40),
                _sched_switch(3000, 't4', 40, 't3', 30),
                _sched_switch(4000, 't3', 30, 't4', 40))),
        query='''
          SELECT
            ts,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS conflict_count
          FROM sched
          ORDER BY ts;
        ''',
        out=Csv('''
        "ts","conflict_count"
        1000,1
        2000,1
        3000,1
        4000,1
        '''))

  # Back-to-back: no overlap, nothing dropped.
  def test_lease_back_to_back(self):
    return DiffTestBlueprint(
        trace=_two_trace_zip(
            _ftrace(
                _sched_switch(100, 't1', 10, 't2', 20),
                _sched_switch(200, 't2', 20, 't1', 10)),
            _ftrace(
                _sched_switch(300, 't3', 30, 't4', 40),
                _sched_switch(400, 't4', 40, 't3', 30))),
        query='''
          SELECT
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS conflict_count;
        ''',
        out=Csv('''
        "sched_count","conflict_count"
        4,0
        '''))

  # 3 traces chain: T1 -> T3 takes over after T1 ends; T2's overlapping events dropped.
  def test_lease_three_traces_chain(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            't1.pb':
                TextProto(
                    _ftrace(
                        _sched_switch(100, 't1', 10, 't2', 20),
                        _sched_switch(200, 't2', 20, 't1', 10))),
            't2.pb':
                TextProto(
                    _ftrace(
                        _sched_switch(150, 't3', 30, 't4', 40),
                        _sched_switch(350, 't4', 40, 't3', 30))),
            't3.pb':
                TextProto(
                    _ftrace(
                        _sched_switch(300, 't5', 50, 't6', 60),
                        _sched_switch(500, 't6', 60, 't5', 50))),
        }),
        query='''
          SELECT
            ts,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS conflict_count
          FROM sched
          ORDER BY ts;
        ''',
        out=Csv('''
        "ts","conflict_count"
        100,2
        200,2
        300,2
        500,2
        '''))

  # Reversed file order gives same output.
  def test_lease_reversed_file_order(self):
    return DiffTestBlueprint(
        trace=_two_trace_zip(
            _ftrace(
                _sched_switch(1000, 't1', 10, 't2', 20),
                _sched_switch(2000, 't2', 20, 't1', 10)),
            _ftrace(
                _sched_switch(1500, 't3', 30, 't4', 40),
                _sched_switch(3000, 't4', 40, 't3', 30)),
            name1='z_t1.pftrace',
            name2='a_t2.pftrace'),
        query='''
          SELECT
            ts,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS conflict_count
          FROM sched
          ORDER BY ts;
        ''',
        out=Csv('''
        "ts","conflict_count"
        1000,1
        2000,1
        3000,1
        '''))

  # Sched in T1, ftrace cpufreq in T2 -> both kept.
  def test_lease_r4a_sched_and_cpufreq(self):
    return DiffTestBlueprint(
        trace=_two_trace_zip(
            _ftrace(
                _sched_switch(1000, 't1', 10, 't2', 20),
                _sched_switch(2000, 't2', 20, 't1', 10)),
            _ftrace(_cpu_freq(1000, 1000000), _cpu_freq(2000, 1200000))),
        query='''
          SELECT
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT count(*) FROM counter) AS counter_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS sched_conflicts,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_counter_claim_conflict') AS counter_conflicts;
        ''',
        out=Csv('''
        "sched_count","counter_count","sched_conflicts","counter_conflicts"
        2,2,0,0
        '''))

  # Sched in T1, atrace slices in T2 -> both kept.
  def test_lease_r4b_sched_and_atrace_slices(self):
    t2 = """
        packet {
          timestamp: 1000
          trusted_packet_sequence_id: 1
          track_event {
            track_uuid: 1
            categories: "cat"
            name: "slice1"
            type: TYPE_SLICE_BEGIN
          }
        }
        packet {
          timestamp: 2000
          trusted_packet_sequence_id: 1
          track_event {
            track_uuid: 1
            type: TYPE_SLICE_END
          }
        }
        """
    return DiffTestBlueprint(
        trace=_two_trace_zip(
            _ftrace(
                _sched_switch(1000, 't1', 10, 't2', 20),
                _sched_switch(2000, 't2', 20, 't1', 10)), t2),
        query='''
          SELECT
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT count(*) FROM slice) AS slice_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS sched_conflicts;
        ''',
        out=Csv('''
        "sched_count","slice_count","sched_conflicts"
        2,1,0
        '''))

  # Meminfo in T1, vmstat in T2 -> both kept.
  def test_lease_r5_meminfo_and_vmstat(self):
    t1 = """
        packet {
          timestamp: 1000
          sys_stats {
            meminfo {
              key: MEMINFO_MEM_TOTAL
              value: 1000000
            }
          }
        }
        """
    t2 = """
        packet {
          timestamp: 1000
          sys_stats {
            vmstat {
              key: VMSTAT_NR_FREE_PAGES
              value: 50000
            }
          }
        }
        """
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query='''
          SELECT
            (SELECT count(*) FROM counter) AS counter_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_counter_claim_conflict') AS counter_conflicts;
        ''',
        out=Csv('''
        "counter_count","counter_conflicts"
        2,0
        '''))

  # Waking-only trace fails closed: waking events after other trace's sched
  # lease are dropped while another trace owns sched.
  def test_lease_waking_only_trace_fail_closed(self):
    return DiffTestBlueprint(
        trace=_two_trace_zip(
            _ftrace(
                _sched_switch(1000, 't1', 10, 't2', 20),
                _sched_switch(3000, 't2', 20, 't1', 10)),
            _ftrace(
                _sched_waking(3500, 20, 't1', 10),
                _sched_waking(4000, 20, 't1', 10))),
        query='''
          SELECT
            (SELECT count(*) FROM thread_state) AS state_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS sched_conflicts;
        ''',
        out=Csv('''
        "state_count","sched_conflicts"
        4,2
        '''))

  # Soft drop: T2 ftrace events before valid-data start don't start claim.
  def test_lease_soft_drop(self):
    t1 = _ftrace(_cpu_freq(1000, 1000000))
    t2 = """
        packet {
          ftrace_events {
            cpu: 0
            previous_bundle_end_timestamp: 1500
            event {
              timestamp: 500
              pid: 1
              cpu_frequency { state: 800000 cpu_id: 0 }
            }
            event {
              timestamp: 2000
              pid: 1
              cpu_frequency { state: 1200000 cpu_id: 0 }
            }
          }
        }
        """
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query='''
          SELECT
            c.ts,
            c.value
          FROM counter c
          JOIN cpu_counter_track t ON c.track_id = t.id
          WHERE t.type = 'cpu_frequency'
          ORDER BY c.ts;
        ''',
        out=Csv('''
        "ts","value"
        1000,1000000.000000
        2000,1200000.000000
        '''))

  # Stream stops early: T1 ftrace ends early, T2 takes over.
  def test_lease_stream_stops_early(self):
    return DiffTestBlueprint(
        trace=_two_trace_zip(
            _ftrace(_cpu_freq(1000, 1000000), _cpu_freq(1500, 1200000)),
            _ftrace(_cpu_freq(2000, 1400000))),
        query='''
          SELECT
            c.ts,
            c.value,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_counter_claim_conflict') AS conflict_count
          FROM counter c
          JOIN cpu_counter_track t ON c.track_id = t.id
          WHERE t.type = 'cpu_frequency'
          ORDER BY c.ts;
        ''',
        out=Csv('''
        "ts","value","conflict_count"
        1000,1000000.000000,0
        1500,1200000.000000,0
        2000,1400000.000000,0
        '''))

  # Handover closing: old owner's open slice closed at its sched lease end.
  def test_lease_handover_closing(self):
    return DiffTestBlueprint(
        trace=_two_trace_zip(
            _ftrace(
                _sched_switch(1000, 't1', 10, 't2', 20),
                _sched_switch(1800, 't2', 20, 't1', 10),
                _sched_waking(2000, 10, 't1', 10)),
            _ftrace(
                _sched_switch(2500, 't3', 30, 't4', 40),
                _sched_switch(3000, 't4', 40, 't3', 30))),
        query='''
          SELECT
            ts,
            dur
          FROM sched
          ORDER BY ts;
        ''',
        out=Csv('''
        "ts","dur"
        1000,800
        1800,200
        2500,500
        3000,-1
        '''))

  # Counter value at expiry: expiry triggered by another track writes cached
  # dropped value at expiry time.
  def test_lease_counter_value_at_expiry(self):
    return DiffTestBlueprint(
        trace=_two_trace_zip(
            _ftrace(
                _cpu_freq(1000, 1000000, cpu=0),
                _cpu_freq(2000, 1200000, cpu=0)),
            _ftrace(
                _cpu_freq(1500, 1500000, cpu=0),
                _cpu_freq(3000, 1800000, cpu=1))),
        query='''
          SELECT
            t.cpu,
            c.ts,
            c.value
          FROM counter c
          JOIN cpu_counter_track t ON c.track_id = t.id
          WHERE t.type = 'cpu_frequency'
          ORDER BY t.cpu, c.ts;
        ''',
        out=Csv('''
        "cpu","ts","value"
        0,1000,1000000.000000
        0,2000,1200000.000000
        1,3000,1800000.000000
        '''))

  # Early waking in trace 2 is closed at trace 1's first switch. Trace 2 later
  # takes over after trace 1's lease ends without creating thread state overlaps.
  def test_early_waking_closed_before_takeover(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            't1.pb':
                TextProto(
                    _ftrace(
                        _sched_switch(2000, 't1', 10, 't2', 20, prev_state=1),
                        _sched_switch(3000, 't2', 20, 't1', 10, prev_state=1),
                    )),
            't2.pb':
                TextProto(
                    _ftrace(
                        _sched_waking(1000, 1, 't2', 20),
                        _sched_switch(2500, 't3', 30, 't2', 20, prev_state=1),
                        _sched_switch(4000, 't3', 30, 't2', 20, prev_state=1),
                        _sched_switch(5000, 't2', 20, 't3', 30, prev_state=1),
                    )),
        }),
        query='''
          SELECT
            (SELECT count(*) FROM thread_state a
             JOIN thread_state b ON a.utid = b.utid AND b.ts > a.ts
             AND (a.dur = -1 OR a.ts + a.dur > b.ts)) AS overlap_count,
            (SELECT count(*) FROM thread_state WHERE dur = -1 AND state = 'R') AS open_r_count,
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS sched_conflicts;
        ''',
        out=Csv('''
        "overlap_count","open_r_count","sched_count","sched_conflicts"
        0,0,4,1
        '''))

  # Waking-only trace: early waking before first switch is closed at claim;
  # wakings after owner's lease ends are dropped while trace 1 owns sched.
  def test_waking_only_trace_dropped_while_owned(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            't1.pb':
                TextProto(
                    _ftrace(
                        _sched_switch(2000, 't1', 10, 't2', 20, prev_state=1),
                        _sched_switch(3000, 't2', 20, 't1', 10, prev_state=1),
                        _sched_switch(4000, 't1', 10, 't2', 20, prev_state=1),
                    )),
            't2.pb':
                TextProto(
                    _ftrace(
                        _sched_waking(1500, 1, 't2', 20),
                        _sched_waking(4500, 1, 't2', 20),
                        _sched_waking(4600, 1, 't1', 10),
                    )),
        }),
        query='''
          SELECT
            (SELECT count(*) FROM thread_state a
             JOIN thread_state b ON a.utid = b.utid AND b.ts > a.ts
             AND (a.dur = -1 OR a.ts + a.dur > b.ts)) AS overlap_count,
            (SELECT count(*) FROM thread_state WHERE dur = -1 AND state = 'R') AS open_r_count,
            (SELECT count(*) FROM thread_state) AS state_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS sched_conflicts;
        ''',
        out=Csv('''
        "overlap_count","open_r_count","state_count","sched_conflicts"
        0,0,7,2
        '''))

  # Three traces: T1 claims, T2 wakings and switches dropped, T3 takes over after
  # T1's lease ends. No overlapping thread states.
  def test_waking_between_two_owners_dropped(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            't1.pb':
                TextProto(
                    _ftrace(
                        _sched_switch(1000, 't1', 10, 't2', 20, prev_state=1),
                        _sched_switch(2000, 't2', 20, 't1', 10, prev_state=1),
                    )),
            't2.pb':
                TextProto(
                    _ftrace(
                        _sched_waking(1100, 1, 't2', 20),
                        _sched_waking(2050, 1, 't2', 20),
                        _sched_switch(2200, 't3', 30, 't2', 20, prev_state=1),
                        _sched_switch(2800, 't2', 20, 't3', 30, prev_state=1),
                    )),
            't3.pb':
                TextProto(
                    _ftrace(
                        _sched_switch(2100, 't4', 40, 't2', 20, prev_state=1),
                        _sched_switch(5000, 't2', 20, 't4', 40, prev_state=1),
                    )),
        }),
        query='''
          SELECT
            (SELECT count(*) FROM thread_state a
             JOIN thread_state b ON a.utid = b.utid AND b.ts > a.ts
             AND (a.dur = -1 OR a.ts + a.dur > b.ts)) AS overlap_count,
            (SELECT count(*) FROM thread_state WHERE dur = -1 AND state = 'R') AS open_r_count,
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS sched_conflicts;
        ''',
        out=Csv('''
        "overlap_count","open_r_count","sched_count","sched_conflicts"
        0,0,4,4
        '''))

  # Generic kernel task states have no sched closer; ownership never hands over,
  # preventing overlapping sched slices on CPU 0.
  def test_generic_kernel_owner_keeps_sched(self):
    t1 = """
        packet {
          timestamp: 1000
          generic_kernel_task_state_event {
            cpu: 0 comm: "t2" tid: 20 state: 3 prio: 100
          }
        }
        packet {
          timestamp: 2000
          generic_kernel_task_state_event {
            cpu: 0 comm: "t2" tid: 20 state: 4 prio: 100
          }
        }
        packet {
          timestamp: 2000
          generic_kernel_task_state_event {
            cpu: 0 comm: "t1" tid: 10 state: 3 prio: 100
          }
        }
        """
    t2 = """
        packet {
          timestamp: 3000
          generic_kernel_task_state_event {
            cpu: 0 comm: "t4" tid: 40 state: 3 prio: 100
          }
        }
        packet {
          timestamp: 3500
          generic_kernel_task_state_event {
            cpu: 0 comm: "t4" tid: 40 state: 4 prio: 100
          }
        }
        packet {
          timestamp: 3500
          generic_kernel_task_state_event {
            cpu: 0 comm: "t3" tid: 30 state: 3 prio: 100
          }
        }
        """
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query='''
          SELECT
            (SELECT count(*) FROM sched a
             JOIN sched b ON a.cpu = b.cpu AND b.ts > a.ts
             AND (a.dur = -1 OR a.ts + a.dur > b.ts)) AS sched_overlaps,
            (SELECT count(*) FROM sched) AS sched_count,
            (SELECT coalesce(sum(value), 0) FROM stats
             WHERE name = 'machine_sched_claim_conflict') AS sched_conflicts;
        ''',
        out=Csv('''
        "sched_overlaps","sched_count","sched_conflicts"
        0,2,3
        '''))
