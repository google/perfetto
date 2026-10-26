#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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

import json

from python.generators.diff_tests.testing import Csv, TextProto, Zip
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite

# Tests for merging traces which were recorded on the same machine and which
# both contain machine-wide data (see MachineDataClaimTracker).
#
# None of the traces below have a machine id or a manifest, so every trace of
# an archive is attributed to the same (host) machine. None have clock
# snapshots either: all timestamps are BOOTTIME, which the traces share.


def _sched_switch(ts, prev_pid, next_pid):
  comm = lambda pid: 'swapper/0' if pid == 0 else f'task_{pid}'
  return f'''
      event {{
        timestamp: {ts}
        pid: {prev_pid}
        sched_switch {{
          prev_comm: "{comm(prev_pid)}"
          prev_pid: {prev_pid}
          prev_prio: 120
          prev_state: 1
          next_comm: "{comm(next_pid)}"
          next_pid: {next_pid}
          next_prio: 120
        }}
      }}'''


def _cpu_frequency(ts, khz):
  return f'''
      event {{
        timestamp: {ts}
        pid: 0
        cpu_frequency {{ cpu_id: 0 state: {khz} }}
      }}'''


def _irq_entry(ts):
  return f'''
      event {{
        timestamp: {ts}
        pid: 0
        irq_handler_entry {{ irq: 5 name: "irq_dev" }}
      }}'''


def _ftrace(*events):
  return TextProto(f'''
    packet {{
      ftrace_events {{
        cpu: 0
        {''.join(events)}
      }}
    }}
  ''')


def _meminfo(ts, value):
  return f'''
    packet {{
      timestamp: {ts}
      sys_stats {{ meminfo {{ key: MEMINFO_MEM_FREE value: {value} }} }}
    }}'''


def _power_rails(descriptor_ts, energies):
  ts_field = f'timestamp: {descriptor_ts}' if descriptor_ts else ''
  data = ''.join(f'''
      packet {{
        timestamp: {ts_ms * 1000000}
        power_rails {{
          energy_data {{ index: 0 timestamp_ms: {ts_ms} energy: {energy} }}
        }}
      }}''' for ts_ms, energy in energies)
  return TextProto(f'''
      packet {{
        {ts_field}
        power_rails {{
          rail_descriptor {{
            index: 0
            rail_name: "CPU_RAIL"
            subsys_name: "cpu"
            sampling_rate: 1000
          }}
        }}
      }}
      {data}
  ''')


# A trace whose kernel data spans [1000, 3000]. The sched slice of task_11
# starting at 2000 and the irq slice starting at 2800 are still open at the
# end of the trace.
TRACE_A = _ftrace(
    _sched_switch(1000, 0, 10),
    _cpu_frequency(1500, 1000000),
    _sched_switch(2000, 10, 11),
    _irq_entry(2800),
    _cpu_frequency(3000, 1100000),
)

# A trace recorded after TRACE_A, spanning [5000, 6000].
TRACE_B_AFTER = _ftrace(
    _sched_switch(5000, 0, 20),
    _cpu_frequency(5500, 2000000),
    _sched_switch(6000, 20, 21),
)

# A trace overlapping the end of TRACE_A, spanning [2500, 4000].
TRACE_B_OVERLAPPING = _ftrace(
    _sched_switch(2500, 0, 20),
    _cpu_frequency(3500, 2000000),
    _sched_switch(4000, 20, 21),
)

_SCHED_QUERY = '''
  SELECT s.ts, s.dur, t.tid
  FROM sched AS s
  JOIN thread AS t USING (utid)
  ORDER BY s.ts;
'''

_FREQ_QUERY = '''
  SELECT
    (SELECT COUNT(*) FROM track WHERE type = 'cpu_frequency') AS tracks,
    c.ts,
    CAST(c.value AS INT) AS value
  FROM counter AS c
  JOIN track AS t ON c.track_id = t.id
  WHERE t.type = 'cpu_frequency'
  ORDER BY c.ts;
'''

_DROPS_QUERY = '''
  SELECT
    f.name AS trace,
    l.ts,
    extract_arg(l.arg_set_id, 'kind') AS kind,
    (
      SELECT name FROM __intrinsic_trace_file
      WHERE id = extract_arg(l.arg_set_id, 'conflicting_trace_id')
    ) AS conflicting_trace,
    (
      SELECT SUM(value) FROM stats
      WHERE name = 'machine_data_claimed_by_other_trace'
    ) AS dropped
  FROM _trace_import_logs AS l
  JOIN __intrinsic_trace_file AS f ON l.trace_id = f.id
  WHERE l.name = 'machine_data_claimed_by_other_trace'
  ORDER BY f.name, kind;
'''


class MergedMachineData(TestSuite):

  # Traces recorded one after the other: both contribute their kernel data,
  # onto shared per-CPU tracks.
  def test_non_overlapping_share_cpu_tracks(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pb': TRACE_A,
            'b.pb': TRACE_B_AFTER,
        }),
        query=_FREQ_QUERY,
        out=Csv('''
          "tracks","ts","value"
          1,1500,1000000
          1,3000,1100000
          1,5500,2000000
        '''))

  # The sched slice left open at the end of the first trace is closed at the
  # end of its window instead of extending over the second trace. The last
  # slice of the second (last) trace stays open, as for a single trace.
  def test_non_overlapping_sched(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pb': TRACE_A,
            'b.pb': TRACE_B_AFTER,
        }),
        query=_SCHED_QUERY,
        out=Csv('''
          "ts","dur","tid"
          1000,1000,10
          2000,1000,11
          5000,1000,20
          6000,-1,21
        '''))

  # Same for thread states.
  def test_non_overlapping_thread_state(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pb': TRACE_A,
            'b.pb': TRACE_B_AFTER,
        }),
        query='''
          SELECT ts.ts, ts.dur, t.tid, ts.state
          FROM thread_state AS ts
          JOIN thread AS t USING (utid)
          WHERE t.tid != 0
          ORDER BY ts.ts, t.tid;
        ''',
        out=Csv('''
          "ts","dur","tid","state"
          1000,1000,10,"Running"
          2000,1000,10,"S"
          2000,1000,11,"Running"
          5000,1000,20,"Running"
          6000,-1,20,"S"
          6000,-1,21,"Running"
        '''))

  # Unfinished slices on shared machine-wide tracks are closed at the end of
  # their trace's window too.
  def test_non_overlapping_closes_open_slices(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pb': TRACE_A,
            'b.pb': TRACE_B_AFTER,
        }),
        query='''
          SELECT s.ts, s.dur, s.name
          FROM slice AS s
          ORDER BY s.ts;
        ''',
        out=Csv('''
          "ts","dur","name"
          2800,200,"IRQ (irq_dev)"
        '''))

  # Order of the traces in the archive doesn't matter for non-overlapping
  # traces: the earlier trace in time is closed at the end of its window.
  def test_non_overlapping_reverse_order(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pb': TRACE_B_AFTER,
            'b.pb': TRACE_A,
        }),
        query=_SCHED_QUERY,
        out=Csv('''
          "ts","dur","tid"
          1000,1000,10
          2000,1000,11
          5000,1000,20
          6000,-1,21
        '''))

  def test_non_overlapping_no_drops(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pb': TRACE_A,
            'b.pb': TRACE_B_AFTER,
        }),
        query='''
          SELECT COUNT(*) AS drops FROM stats
          WHERE name = 'machine_data_claimed_by_other_trace' AND value > 0;
        ''',
        out=Csv('''
          "drops"
          0
        '''))

  # Every row closed at a trace boundary (above: one sched slice, two thread
  # states and one slice) is counted against the trace it belongs to.
  def test_non_overlapping_closed_at_boundary_stat(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pb': TRACE_A,
            'b.pb': TRACE_B_AFTER,
        }),
        query='''
          SELECT f.name AS trace, s.value
          FROM stats AS s
          JOIN __intrinsic_trace_file AS f ON s.trace_id = f.id
          WHERE s.name = 'machine_data_closed_at_trace_boundary'
          ORDER BY f.name;
        ''',
        out=Csv('''
          "trace","value"
          "a.pb",4
          "b.pb",0
        '''))

  # Overlapping traces: the data of the later trace which overlaps the first
  # trace's window is dropped; the rest is kept.
  def test_overlapping_sched(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pb': TRACE_A,
            'b.pb': TRACE_B_OVERLAPPING,
        }),
        query=_SCHED_QUERY,
        out=Csv('''
          "ts","dur","tid"
          1000,1000,10
          2000,1000,11
          4000,-1,21
        '''))

  def test_overlapping_cpu_frequency(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pb': TRACE_A,
            'b.pb': TRACE_B_OVERLAPPING,
        }),
        query=_FREQ_QUERY,
        out=Csv('''
          "tracks","ts","value"
          1,1500,1000000
          1,3000,1100000
          1,3500,2000000
        '''))

  # The drop is counted and explained once in the import logs.
  def test_overlapping_logs_drop(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pb': TRACE_A,
            'b.pb': TRACE_B_OVERLAPPING,
        }),
        query=_DROPS_QUERY,
        out=Csv('''
          "trace","ts","kind","conflicting_trace","dropped"
          "b.pb",2500,"kernel","a.pb",1
        '''))

  # Different kinds of machine-wide data recorded at the same time by
  # different traces (e.g. ftrace and sys_stats) are all kept.
  def test_different_kinds_overlapping_are_kept(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pb': TRACE_A,
            'b.pb': TextProto(_meminfo(1500, 100) + _meminfo(2500, 200)),
        }),
        query='''
          SELECT
            (SELECT COUNT(*) FROM sched) AS sched_slices,
            (
              SELECT GROUP_CONCAT(CAST(c.value AS INT), ',')
              FROM counter AS c
              JOIN track AS t ON c.track_id = t.id
              WHERE t.type = 'meminfo'
            ) AS meminfo,
            (
              SELECT COUNT(*) FROM stats
              WHERE name = 'machine_data_claimed_by_other_trace' AND value > 0
            ) AS drops;
        ''',
        out=Csv('''
          "sched_slices","meminfo","drops"
          2,"102400,204800",0
        '''))

  # The same kind of data from two traces overlapping: sys_stats.
  def test_sys_stats_overlapping(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pb':
                TextProto(_meminfo(1000, 100) + _meminfo(2000, 200)),
            'b.pb':
                TextProto(
                    _meminfo(1500, 900) + _meminfo(2000, 900) +
                    _meminfo(3000, 300)),
        }),
        query='''
          SELECT
            (SELECT COUNT(*) FROM track WHERE type = 'meminfo') AS tracks,
            c.ts,
            CAST(c.value AS INT) AS value
          FROM counter AS c
          JOIN track AS t ON c.track_id = t.id
          WHERE t.type = 'meminfo'
          ORDER BY c.ts;
        ''',
        out=Csv('''
          "tracks","ts","value"
          1,1000,102400
          1,2000,204800
          1,3000,307200
        '''))

  # Non machine-wide data of the overlapping trace (here a track event slice)
  # is unaffected.
  def test_overlapping_keeps_other_data(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pb':
                TRACE_A,
            'b.pb':
                TextProto('''
                  packet {
                    trusted_packet_sequence_id: 1
                    track_descriptor { uuid: 1 }
                  }
                  packet {
                    trusted_packet_sequence_id: 1
                    timestamp: 1500
                    track_event {
                      type: TYPE_SLICE_BEGIN
                      track_uuid: 1
                      name: "sdk_slice"
                    }
                  }
                  packet {
                    trusted_packet_sequence_id: 1
                    timestamp: 1700
                    track_event { type: TYPE_SLICE_END track_uuid: 1 }
                  }
                  packet {
                    ftrace_events {
                      cpu: 0
                      event {
                        timestamp: 1600
                        pid: 0
                        cpu_frequency { cpu_id: 0 state: 5 }
                      }
                    }
                  }
                '''),
        }),
        query='''
          SELECT s.ts, s.dur, s.name
          FROM slice AS s
          WHERE s.name = 'sdk_slice';
        ''',
        out=Csv('''
          "ts","dur","name"
          1500,200,"sdk_slice"
        '''))

  # Traces attributed to different machines never conflict.
  def test_different_machines_are_independent(self):
    return DiffTestBlueprint(
        trace=Zip({
            'meta.json':
                json.dumps({
                    'perfetto_manifest': {
                        'version':
                            1,
                        'files': [
                            {
                                'path': 'a.pb',
                                'machine': {
                                    'name': 'phone_a'
                                }
                            },
                            {
                                'path': 'b.pb',
                                'machine': {
                                    'name': 'phone_b'
                                }
                            },
                        ],
                    }
                }),
            'a.pb':
                TRACE_A,
            'b.pb':
                TRACE_B_OVERLAPPING,
        }),
        query='''
          SELECT
            (SELECT COUNT(*) FROM track WHERE type = 'cpu_frequency')
              AS freq_tracks,
            (SELECT COUNT(*) FROM sched) AS sched_slices,
            (
              SELECT COUNT(*) FROM stats
              WHERE name = 'machine_data_claimed_by_other_trace' AND value > 0
            ) AS drops;
        ''',
        out=Csv('''
          "freq_tracks","sched_slices","drops"
          2,4,0
        '''))

  # Systrace text is kernel data too: a systrace overlapping a proto trace
  # with ftrace is dropped where it overlaps.
  def test_systrace_overlapping_proto(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pb':
                _ftrace(
                    _cpu_frequency(1000000000, 1000),
                    _cpu_frequency(2000000000, 2000),
                ),
            'b.systrace':
                '''# tracer: nop
#
          <idle>-0     (-----) [000] d..3  1.500000: cpu_frequency: state=9 cpu_id=0
          <idle>-0     (-----) [000] d..3  3.000000: cpu_frequency: state=3000 cpu_id=0
''',
        }),
        query=_FREQ_QUERY,
        out=Csv('''
          "tracks","ts","value"
          1,1000000000,1000
          1,2000000000,2000
          1,3000000000,3000
        '''))

  # Power rail descriptors are kept even when they fall in another trace's
  # window: only the energy data is arbitrated.
  def test_power_rails(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pb': _power_rails(None, [(1, 100), (2, 200)]),
            'b.pb': _power_rails(1500000, [(1, 900), (3, 300), (4, 400)]),
        }),
        query='''
          SELECT
            (SELECT COUNT(*) FROM track WHERE name GLOB '*CPU_RAIL*') AS tracks,
            t.name,
            c.ts,
            CAST(c.value AS INT) AS value
          FROM counter AS c
          JOIN track AS t ON c.track_id = t.id
          WHERE t.name GLOB '*CPU_RAIL*'
          ORDER BY c.ts;
        ''',
        out=Csv('''
          "tracks","name","ts","value"
          1,"power.CPU_RAIL_uws",1000000,100
          1,"power.CPU_RAIL_uws",2000000,200
          1,"power.CPU_RAIL_uws",3000000,300
          1,"power.CPU_RAIL_uws",4000000,400
        '''))

  # A single trace is unaffected: its open slices stay open.
  def test_single_trace_unchanged(self):
    return DiffTestBlueprint(
        trace=TRACE_A,
        query=_SCHED_QUERY,
        out=Csv('''
          "ts","dur","tid"
          1000,1000,10
          2000,-1,11
        '''))
