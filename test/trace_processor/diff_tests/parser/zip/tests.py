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

import io
import json
import zipfile

from python.generators.diff_tests.testing import Csv, Path, DataPath
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from python.generators.diff_tests.testing import TextProto
from python.generators.diff_tests.testing import Tar
from python.generators.diff_tests.testing import Zip as ZipTrace


def _ftrace(*events, cpu=0):
  return f'packet {{ ftrace_events {{ cpu: {cpu} {" ".join(events)} }} }}'


def _cpu_freq(ts, state, cpu=0):
  return (f'event {{ timestamp: {ts} pid: 1 cpu_frequency {{ '
          f'state: {state} cpu_id: {cpu} }} }}')


def _two_trace_zip(trace1,
                   trace2=None,
                   name1='trace1.pftrace',
                   name2='trace2.pftrace'):
  """ZIP with trace1.pftrace and trace2.pftrace (a copy of trace1 if None)."""
  return ZipTrace({
      name1: TextProto(trace1),
      name2: TextProto(trace1 if trace2 is None else trace2),
  })


def _make_bugreport_zip(dumpstate_txt: str) -> bytes:
  buf = io.BytesIO()
  with zipfile.ZipFile(buf, 'w') as z:
    z.writestr('bugreport-test-2022-06-24-16-24-37.txt', dumpstate_txt)
  return buf.getvalue()


_MACHINE_SCOPED_TYPES = (
    'android_cpu_per_uid_totals',
    'android_dma_heap',
    'android_energy_estimation_breakdown',
    'android_gpu_work_period',
    'android_kernel_wakelock',
    'battery_counter',
    'battery_stats',
    'battery_status',
    'buddyinfo',
    'clock_frequency',
    'clock_state',
    'cooling_device_counter',
    'cpu_capacity',
    'cpu_frequency',
    'cpu_frequency_throttle',
    'cpu_idle',
    'cpu_idle_state',
    'cpu_max_frequency_limit',
    'cpu_min_frequency_limit',
    'cpu_nr_running',
    'cpu_utilization',
    'cpustat',
    'entity_state',
    'gpu_frequency',
    'gpu_memory',
    'ion',
    'meminfo',
    'num_forks',
    'num_irq',
    'num_irq_total',
    'num_softirq',
    'num_softirq_total',
    'plug_type',
    'psi',
    'screen_state',
    'slabinfo',
    'thermal_temperature',
    'thermal_temperature_sys',
    'vmstat',
)
_MACHINE_SCOPED_TYPES_SQL = ', '.join(f"'{t}'" for t in _MACHINE_SCOPED_TYPES)


def _machine_query(track_type='cpu_frequency',
                   track_table='cpu_counter_track',
                   track_filter=None):
  where_counter = f"t.type = '{track_type}'" if track_type else '1=1'
  where_stat = f"t.type = '{track_type}'" if track_type else '1=1'
  if track_filter:
    where_counter += f' AND {track_filter}'
    where_stat += f' AND {track_filter}'
  return f"""
    SELECT
      'counter' AS section,
      c.ts,
      c.value,
      t.name AS track_name,
      NULL AS trace_name,
      (SELECT count(*) FROM (
         SELECT type, dimension_arg_set_id, machine_id
         FROM track
         WHERE type IN ({_MACHINE_SCOPED_TYPES_SQL})
         GROUP BY 1,2,3
         HAVING count(*) > 1
       )) AS duplicate_tracks
    FROM counter c
    JOIN {track_table} t ON c.track_id = t.id
    WHERE {where_counter}

    UNION ALL

    SELECT
      'stat' AS section,
      NULL AS ts,
      s.value,
      t.name AS track_name,
      tf.name AS trace_name,
      NULL AS duplicate_tracks
    FROM stats s
    JOIN __intrinsic_trace_file tf ON s.trace_id = tf.id
    JOIN track t ON s.idx = t.id
    WHERE s.name = 'machine_track_events_hidden' AND {where_stat}

    UNION ALL

    SELECT
      'import_log' AS section,
      l.ts,
      NULL AS value,
      t.name AS track_name,
      printf('%s owner=%s', tf_writer.name, tf_owner.name) AS trace_name,
      NULL AS duplicate_tracks
    FROM _trace_import_logs l
    JOIN __intrinsic_trace_file tf_writer ON l.trace_id = tf_writer.id
    JOIN args a_owner ON l.arg_set_id = a_owner.arg_set_id AND a_owner.key = 'owner_trace_id'
    JOIN __intrinsic_trace_file tf_owner ON a_owner.int_value = tf_owner.id
    JOIN args a_track ON l.arg_set_id = a_track.arg_set_id AND a_track.key = 'track_id'
    JOIN track t ON a_track.int_value = t.id
    WHERE l.name = 'machine_track_events_hidden' AND {where_stat}
    ORDER BY section, track_name, ts, trace_name;
  """


_MACHINE_SCOPED_TABLE = [
    ('android_cpu_per_uid_totals', '''
      packet {
        timestamp: 1000
        cpu_per_uid_data { cluster_count: 1 uid: 0 total_time_ms: 100 }
      }
    '''),
    ('android_dma_heap', '''
      packet {
        ftrace_events {
          cpu: 0
          event { timestamp: 1000 pid: 1 dma_heap_stat { total_allocated: 4096 } }
        }
      }
    '''),
    ('android_energy_estimation_breakdown', '''
      packet {
        android_energy_estimation_breakdown {
          energy_consumer_descriptor {
            energy_consumers { energy_consumer_id: 0 name: "CPU" }
          }
        }
      }
      packet {
        timestamp: 1000
        android_energy_estimation_breakdown { energy_consumer_id: 0 energy_uws: 1000 }
      }
    '''),
    ('android_gpu_work_period', '''
      packet {
        ftrace_events {
          cpu: 0
          event { timestamp: 1000 pid: 1 gpu_work_period { gpu_id: 0 uid: 1000 start_time_ns: 1000 end_time_ns: 2000 total_active_duration_ns: 500 } }
        }
      }
    '''),
    ('android_kernel_wakelock', '''
      packet {
        timestamp: 1000
        trusted_packet_sequence_id: 2
        sequence_flags: 3
        previous_packet_dropped: 1
        first_packet_on_sequence: true
        kernel_wakelock_data {
          wakelock {
            wakelock_id: 1
            wakelock_name: "test_wakelock"
            wakelock_type: WAKELOCK_TYPE_KERNEL
          }
          wakelock_id: 1
          time_held_millis: 10
        }
      }
    '''),
    ('battery_counter', '''
      packet {
        timestamp: 1000
        battery { capacity_percent: 85 }
      }
    '''),
    ('battery_stats', '''
      packet {
        timestamp: 1000
        android_system_property {
          values { name: "debug.tracing.battery_stats.test" value: "100" }
        }
      }
    '''),
    ('battery_status', '''
      packet {
        timestamp: 1000
        android_system_property {
          values { name: "debug.tracing.battery_status" value: "3" }
        }
      }
    '''),
    ('buddyinfo', '''
      packet {
        timestamp: 1000
        sys_stats {
          buddy_info { node: "0" zone: "Normal" order_pages: 5 }
        }
      }
    '''),
    ('clock_frequency', '''
      packet {
        ftrace_events {
          cpu: 0
          event { timestamp: 1000 pid: 1 clock_set_rate { name: "clk0" state: 1000 } }
        }
      }
    '''),
    ('clock_state', '''
      packet {
        ftrace_events {
          cpu: 0
          event { timestamp: 1000 pid: 1 clock_enable { name: "clk0" state: 1 } }
        }
      }
    '''),
    ('cooling_device_counter', '''
      packet {
        ftrace_events {
          cpu: 0
          event { timestamp: 1000 pid: 1 cdev_update { type: "cdev0" target: 1 } }
        }
      }
    '''),
    ('cpu_capacity', '''
      packet {
        ftrace_events {
          cpu: 0
          event { timestamp: 1000 pid: 1 sched_cpu_util_cfs { cpu: 0 cpu_util: 50 capacity: 1024 nr_running: 1 } }
        }
      }
    '''),
    ('cpu_frequency', '''
      packet {
        ftrace_events {
          cpu: 0
          event { timestamp: 1000 pid: 1 cpu_frequency { state: 1000 cpu_id: 0 } }
        }
      }
    '''),
    ('cpu_frequency_throttle', '''
      packet {
        ftrace_events {
          cpu: 0
          event { timestamp: 1000 pid: 1 dcvsh_freq { cpu: 0 freq: 1000 } }
        }
      }
    '''),
    ('cpu_idle', '''
      packet {
        ftrace_events {
          cpu: 0
          event { timestamp: 1000 pid: 1 cpu_idle { state: 1 cpu_id: 0 } }
        }
      }
    '''),
    ('cpu_idle_state', '''
      packet {
        timestamp: 1000
        sys_stats {
          cpuidle_state { cpu_id: 0 cpuidle_state_entry { state: "C1" duration_us: 100 } }
        }
      }
    '''),
    ('cpu_max_frequency_limit', '''
      packet {
        ftrace_events {
          cpu: 0
          event { timestamp: 1000 pid: 1 cpu_frequency_limits { max_freq: 2000 min_freq: 500 cpu_id: 0 } }
        }
      }
    '''),
    ('cpu_min_frequency_limit', '''
      packet {
        ftrace_events {
          cpu: 0
          event { timestamp: 1001 pid: 1 cpu_frequency_limits { max_freq: 2000 min_freq: 600 cpu_id: 0 } }
        }
      }
    '''),
    ('cpu_nr_running', '''
      packet {
        ftrace_events {
          cpu: 0
          event { timestamp: 1002 pid: 1 sched_cpu_util_cfs { cpu: 0 cpu_util: 50 capacity: 1024 nr_running: 1 } }
        }
      }
    '''),
    ('cpu_utilization', '''
      packet {
        ftrace_events {
          cpu: 0
          event { timestamp: 1003 pid: 1 sched_cpu_util_cfs { cpu: 0 cpu_util: 50 capacity: 1024 nr_running: 1 } }
        }
      }
    '''),
    ('cpustat', '''
      packet {
        timestamp: 1000
        sys_stats {
          cpu_stat { cpu_id: 0 user_ns: 100 }
        }
      }
    '''),
    ('entity_state', '''
      packet {
        timestamp: 1000
        entity_state_residency {
          power_entity_state { entity_index: 0 state_index: 0 entity_name: "BT" state_name: "Idle" }
          residency { entity_index: 0 state_index: 0 total_time_in_state_ms: 100 }
        }
      }
    '''),
    ('gpu_frequency', '''
      packet {
        timestamp: 1000
        generic_gpu_frequency_event { gpu_id: 0 frequency_khz: 500000 }
      }
    '''),
    ('gpu_memory', '''
      packet {
        ftrace_events {
          cpu: 0
          event { timestamp: 1000 pid: 0 gpu_mem_total { gpu_id: 0 pid: 0 size: 1024 } }
        }
      }
    '''),
    ('ion', '''
      packet {
        ftrace_events {
          cpu: 0
          event { timestamp: 1000 pid: 1 ion_heap_grow { heap_name: "system" len: 1024 total_allocated: 2048 } }
        }
      }
    '''),
    ('meminfo', '''
      packet {
        timestamp: 1000
        sys_stats {
          meminfo { key: MEMINFO_MEM_FREE value: 1000 }
        }
      }
    '''),
    ('num_forks', '''
      packet {
        timestamp: 1000
        sys_stats { num_forks: 42 }
      }
    '''),
    ('num_irq', '''
      packet {
        timestamp: 1000
        sys_stats { num_irq { irq: 1 count: 10 } }
      }
    '''),
    ('num_irq_total', '''
      packet {
        timestamp: 1000
        sys_stats { num_irq_total: 100 }
      }
    '''),
    ('num_softirq', '''
      packet {
        timestamp: 1000
        sys_stats { num_softirq { irq: 1 count: 20 } }
      }
    '''),
    ('num_softirq_total', '''
      packet {
        timestamp: 1000
        sys_stats { num_softirq_total: 200 }
      }
    '''),
    ('plug_type', '''
      packet {
        timestamp: 1000
        android_system_property {
          values { name: "debug.tracing.plug_type" value: "1" }
        }
      }
    '''),
    ('psi', '''
      packet {
        timestamp: 1000
        sys_stats { psi { resource: PSI_RESOURCE_CPU_SOME total_ns: 1000 } }
      }
    '''),
    ('screen_state', '''
      packet {
        timestamp: 1000
        android_system_property {
          values { name: "debug.tracing.screen_state" value: "2" }
        }
      }
    '''),
    ('slabinfo', '''
      packet {
        timestamp: 1000
        sys_stats { slab_info { name: "test_slab" pages_per_slab: 1 num_slabs: 1 } }
      }
    '''),
    ('thermal_temperature', '''
      packet {
        ftrace_events {
          cpu: 0
          event { timestamp: 1000 pid: 1 thermal_temperature { thermal_zone: "tz0" temp: 42000 } }
        }
      }
    '''),
    ('thermal_temperature_sys', '''
      packet {
        timestamp: 1000
        sys_stats { thermal_zone { type: "cpu-thermal" temp: 45000 } }
      }
    '''),
    ('vmstat', '''
      packet {
        timestamp: 1000
        sys_stats { vmstat { key: VMSTAT_NR_FREE_PAGES value: 500 } }
      }
    '''),
]


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

  # Full overlap: trace1 owns the track; trace2 writes are hidden with 0 duplicate tracks.
  def test_machine_full_overlap_cpufreq(self):
    payload = _ftrace(_cpu_freq(1000, 1000000), _cpu_freq(2000, 1200000))
    return DiffTestBlueprint(
        trace=_two_trace_zip(payload),
        query=_machine_query(),
        out=Csv('''
        "section","ts","value","track_name","trace_name","duplicate_tracks"
        "counter",1000,1000000.000000,"cpufreq","[NULL]",0
        "counter",2000,1200000.000000,"cpufreq","[NULL]",0
        "import_log",1000,"[NULL]","cpufreq","trace2.pftrace owner=trace1.pftrace","[NULL]"
        "stat","[NULL]",2,"cpufreq","trace2.pftrace","[NULL]"
        '''))

  # Table-driven check: all machine-scoped types have 1 track per dimension and hidden events in duplicate trace.
  def test_machine_same_file_twice(self):
    snapshot_header = """
        packet {
          clock_snapshot {
            clocks { clock_id: 5 timestamp: 0 }
            clocks { clock_id: 6 timestamp: 0 }
          }
        }
        """
    payload = snapshot_header + '\n'.join(
        snippet for _, snippet in _MACHINE_SCOPED_TABLE)
    expected_rows = ['"type","tracks_per_dim","hidden_events"']
    for track_type, _ in sorted(_MACHINE_SCOPED_TABLE, key=lambda x: x[0]):
      expected_rows.append(f'"{track_type}",1,1')
    expected_csv = '\n'.join(expected_rows) + '\n'

    return DiffTestBlueprint(
        trace=_two_trace_zip(
            payload, name1='file_a.pftrace', name2='file_b.pftrace'),
        query=f'''
          SELECT
            t.type,
            (SELECT max(c) FROM (
               SELECT count(*) AS c
               FROM track t2
               WHERE t2.type = t.type
               GROUP BY t2.dimension_arg_set_id, t2.machine_id
             )) AS tracks_per_dim,
            (SELECT count(*) > 0 FROM stats s
             JOIN __intrinsic_trace_file tf ON s.trace_id = tf.id
             JOIN track t3 ON s.idx = t3.id
             WHERE s.name = 'machine_track_events_hidden'
               AND t3.type = t.type
               AND tf.name = 'file_b.pftrace') AS hidden_events
          FROM track t
          WHERE t.type IN ({_MACHINE_SCOPED_TYPES_SQL})
          GROUP BY t.type
          ORDER BY t.type;
        ''',
        out=Csv(expected_csv))

  # Partial overlap: A owns up to 3000, B hidden in 2000..3000, then B takes over at 3500.
  def test_machine_partial_overlap(self):
    t1 = _ftrace(
        _cpu_freq(1000, 1000000),
        _cpu_freq(2000, 1100000),
        _cpu_freq(3000, 1200000),
    )
    t2 = _ftrace(
        _cpu_freq(2000, 2000000),
        _cpu_freq(2500, 2500000),
        _cpu_freq(3500, 3500000),
        _cpu_freq(5000, 5000000),
    )
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query=_machine_query(),
        out=Csv('''
        "section","ts","value","track_name","trace_name","duplicate_tracks"
        "counter",1000,1000000.000000,"cpufreq","[NULL]",0
        "counter",2000,1100000.000000,"cpufreq","[NULL]",0
        "counter",3000,1200000.000000,"cpufreq","[NULL]",0
        "counter",3500,3500000.000000,"cpufreq","[NULL]",0
        "counter",5000,5000000.000000,"cpufreq","[NULL]",0
        "import_log",2000,"[NULL]","cpufreq","trace2.pftrace owner=trace1.pftrace","[NULL]"
        "stat","[NULL]",2,"cpufreq","trace2.pftrace","[NULL]"
        '''))

  # Consecutive traces: A (1000..2000) and B (3000..4000) both kept with 0 hidden events.
  def test_machine_consecutive(self):
    t1 = _ftrace(_cpu_freq(1000, 1000000), _cpu_freq(2000, 1200000))
    t2 = _ftrace(_cpu_freq(3000, 1300000), _cpu_freq(4000, 1400000))
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query=_machine_query(),
        out=Csv('''
        "section","ts","value","track_name","trace_name","duplicate_tracks"
        "counter",1000,1000000.000000,"cpufreq","[NULL]",0
        "counter",2000,1200000.000000,"cpufreq","[NULL]",0
        "counter",3000,1300000.000000,"cpufreq","[NULL]",0
        "counter",4000,1400000.000000,"cpufreq","[NULL]",0
        '''))

  # Untimed packets keep their borrowed timestamp without extending their trace's end,
  # ensuring consecutive traces hand over cleanly despite archive order.
  def test_machine_consecutive_archive_order_differs(self):
    untimed = ('packet { process_tree { processes { pid: 10 ppid: 1 '
               'cmdline: "untimed" } } }')
    return DiffTestBlueprint(
        trace=ZipTrace({
            'a_day3.pftrace':
                TextProto(
                    _ftrace(_cpu_freq(6000, 600000), _cpu_freq(7000, 700000))),
            'b_day1.pftrace':
                TextProto(
                    untimed + '\n' +
                    _ftrace(_cpu_freq(1000, 100000), _cpu_freq(2000, 200000))),
            'c_day2.pftrace':
                TextProto(
                    untimed + '\n' +
                    _ftrace(_cpu_freq(3000, 300000), _cpu_freq(4000, 400000))),
        }),
        query='''
          SELECT
            'counter' AS section,
            c.ts,
            c.value,
            NULL AS trace_name
          FROM counter c
          JOIN cpu_counter_track t ON c.track_id = t.id
          WHERE t.type = 'cpu_frequency'

          UNION ALL

          SELECT
            'stat' AS section,
            NULL AS ts,
            s.value,
            f.name AS trace_name
          FROM stats s
          JOIN __intrinsic_trace_file f ON s.trace_id = f.id
          WHERE s.name = 'machine_track_events_hidden'

          UNION ALL

          SELECT
            'tracks' AS section,
            NULL AS ts,
            COUNT(*) AS value,
            NULL AS trace_name
          FROM cpu_counter_track
          WHERE type = 'cpu_frequency'
          ORDER BY section, ts;
        ''',
        out=Csv('''
        "section","ts","value","trace_name"
        "counter",1000,100000.000000,"[NULL]"
        "counter",2000,200000.000000,"[NULL]"
        "counter",3000,300000.000000,"[NULL]"
        "counter",4000,400000.000000,"[NULL]"
        "counter",6000,600000.000000,"[NULL]"
        "counter",7000,700000.000000,"[NULL]"
        "tracks","[NULL]",1,"[NULL]"
        '''))

  # Three chained traces: each owns until its end, then the next takes over.
  def test_machine_three_chained(self):
    t1 = _ftrace(_cpu_freq(1000, 1000000), _cpu_freq(3000, 1200000))
    t2 = _ftrace(
        _cpu_freq(2500, 2500000), _cpu_freq(3500, 3500000),
        _cpu_freq(5000, 5000000))
    t3 = _ftrace(
        _cpu_freq(4500, 4500000), _cpu_freq(5500, 5500000),
        _cpu_freq(7000, 7000000))
    return DiffTestBlueprint(
        trace=ZipTrace({
            'trace1.pftrace': TextProto(t1),
            'trace2.pftrace': TextProto(t2),
            'trace3.pftrace': TextProto(t3),
        }),
        query=_machine_query(),
        out=Csv('''
        "section","ts","value","track_name","trace_name","duplicate_tracks"
        "counter",1000,1000000.000000,"cpufreq","[NULL]",0
        "counter",3000,1200000.000000,"cpufreq","[NULL]",0
        "counter",3500,3500000.000000,"cpufreq","[NULL]",0
        "counter",5000,5000000.000000,"cpufreq","[NULL]",0
        "counter",5500,5500000.000000,"cpufreq","[NULL]",0
        "counter",7000,7000000.000000,"cpufreq","[NULL]",0
        "import_log",2500,"[NULL]","cpufreq","trace2.pftrace owner=trace1.pftrace","[NULL]"
        "import_log",4500,"[NULL]","cpufreq","trace3.pftrace owner=trace2.pftrace","[NULL]"
        "stat","[NULL]",1,"cpufreq","trace2.pftrace","[NULL]"
        "stat","[NULL]",1,"cpufreq","trace3.pftrace","[NULL]"
        '''))

  # B inside A: A owns CPU 0, while B's extra CPU 1 is kept.
  def test_machine_b_inside_a(self):
    t1 = _ftrace(
        _cpu_freq(1000, 1000000, cpu=0),
        _cpu_freq(3000, 1200000, cpu=0),
        _cpu_freq(5000, 1400000, cpu=0),
    )
    t2 = _ftrace(
        _cpu_freq(2000, 2000000, cpu=0),
        _cpu_freq(4000, 2200000, cpu=0),
        _cpu_freq(2000, 3000000, cpu=1),
        _cpu_freq(4000, 3200000, cpu=1),
    )
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query=f'''
          SELECT
            'counter' AS section,
            t.cpu,
            c.ts,
            c.value,
            NULL AS trace_name,
            (SELECT count(*) FROM (
               SELECT type, dimension_arg_set_id, machine_id
               FROM track
               WHERE type IN ({_MACHINE_SCOPED_TYPES_SQL})
               GROUP BY 1,2,3
               HAVING count(*) > 1
             )) AS duplicate_tracks
          FROM counter c
          JOIN cpu_counter_track t ON c.track_id = t.id
          WHERE t.type = 'cpu_frequency'

          UNION ALL

          SELECT
            'stat' AS section,
            t.cpu,
            NULL AS ts,
            s.value,
            tf.name AS trace_name,
            NULL AS duplicate_tracks
          FROM stats s
          JOIN __intrinsic_trace_file tf ON s.trace_id = tf.id
          JOIN cpu_counter_track t ON s.idx = t.id
          WHERE s.name = 'machine_track_events_hidden'
          ORDER BY section, cpu, ts;
        ''',
        out=Csv('''
        "section","cpu","ts","value","trace_name","duplicate_tracks"
        "counter",0,1000,1000000.000000,"[NULL]",0
        "counter",0,3000,1200000.000000,"[NULL]",0
        "counter",0,5000,1400000.000000,"[NULL]",0
        "counter",1,2000,3000000.000000,"[NULL]",0
        "counter",1,4000,3200000.000000,"[NULL]",0
        "stat",0,"[NULL]",2,"trace2.pftrace","[NULL]"
        '''))

  # Counter starts late in A: B starts earlier and owns until B's end, hiding A's late write.
  def test_machine_counter_starts_late(self):
    t1 = """
        packet {
          ftrace_events {
            cpu: 0
            event { timestamp: 1000 pid: 1 sched_switch { prev_comm: "t" prev_pid: 1 prev_state: 0 next_comm: "t" next_pid: 2 next_prio: 120 } }
            event { timestamp: 3000 pid: 1 cpu_frequency { state: 1300000 cpu_id: 0 } }
            event { timestamp: 5000 pid: 1 sched_switch { prev_comm: "t" prev_pid: 2 prev_state: 0 next_comm: "t" next_pid: 1 next_prio: 120 } }
          }
        }
        """
    t2 = """
        packet {
          ftrace_events {
            cpu: 0
            event { timestamp: 1000 pid: 1 sched_switch { prev_comm: "t" prev_pid: 1 prev_state: 0 next_comm: "t" next_pid: 2 next_prio: 120 } }
            event { timestamp: 1500 pid: 1 cpu_frequency { state: 1500000 cpu_id: 0 } }
            event { timestamp: 4000 pid: 1 cpu_frequency { state: 1800000 cpu_id: 0 } }
            event { timestamp: 5000 pid: 1 sched_switch { prev_comm: "t" prev_pid: 2 prev_state: 0 next_comm: "t" next_pid: 1 next_prio: 120 } }
          }
        }
        """
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query=_machine_query(),
        out=Csv('''
        "section","ts","value","track_name","trace_name","duplicate_tracks"
        "counter",1500,1500000.000000,"cpufreq","[NULL]",0
        "counter",4000,1800000.000000,"cpufreq","[NULL]",0
        "import_log",3000,"[NULL]","cpufreq","trace1.pftrace owner=trace2.pftrace","[NULL]"
        "stat","[NULL]",1,"cpufreq","trace1.pftrace","[NULL]"
        '''))

  # A's counter data stops early, but a late event on CPU 1 keeps A's claim through 5000.
  def test_machine_owner_data_stops_early(self):
    t1 = """
        packet {
          ftrace_events {
            cpu: 0
            event { timestamp: 1000 pid: 1 cpu_frequency { state: 1000000 cpu_id: 0 } }
            event { timestamp: 2000 pid: 1 cpu_frequency { state: 1200000 cpu_id: 0 } }
          }
        }
        packet {
          ftrace_events {
            cpu: 1
            event { timestamp: 5000 pid: 1 cpu_frequency { state: 1500000 cpu_id: 1 } }
          }
        }
        """
    t2 = """
        packet {
          ftrace_events {
            cpu: 0
            event { timestamp: 1500 pid: 1 cpu_frequency { state: 1500000 cpu_id: 0 } }
            event { timestamp: 2500 pid: 1 cpu_frequency { state: 1600000 cpu_id: 0 } }
            event { timestamp: 4500 pid: 1 cpu_frequency { state: 1700000 cpu_id: 0 } }
            event { timestamp: 6000 pid: 1 cpu_frequency { state: 1800000 cpu_id: 0 } }
          }
        }
        """
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query=f'''
          SELECT
            'counter' AS section,
            t.cpu,
            c.ts,
            c.value,
            NULL AS trace_name,
            (SELECT count(*) FROM (
               SELECT type, dimension_arg_set_id, machine_id
               FROM track
               WHERE type IN ({_MACHINE_SCOPED_TYPES_SQL})
               GROUP BY 1,2,3
               HAVING count(*) > 1
             )) AS duplicate_tracks
          FROM counter c
          JOIN cpu_counter_track t ON c.track_id = t.id
          WHERE t.type = 'cpu_frequency' AND t.cpu = 0

          UNION ALL

          SELECT
            'stat' AS section,
            t.cpu,
            NULL AS ts,
            s.value,
            tf.name AS trace_name,
            NULL AS duplicate_tracks
          FROM stats s
          JOIN __intrinsic_trace_file tf ON s.trace_id = tf.id
          JOIN cpu_counter_track t ON s.idx = t.id
          WHERE s.name = 'machine_track_events_hidden' AND t.cpu = 0

          UNION ALL

          SELECT
            'import_log' AS section,
            t.cpu,
            l.ts,
            NULL AS value,
            printf('%s owner=%s', tf_writer.name, tf_owner.name) AS trace_name,
            NULL AS duplicate_tracks
          FROM _trace_import_logs l
          JOIN __intrinsic_trace_file tf_writer ON l.trace_id = tf_writer.id
          JOIN args a_owner ON l.arg_set_id = a_owner.arg_set_id AND a_owner.key = 'owner_trace_id'
          JOIN __intrinsic_trace_file tf_owner ON a_owner.int_value = tf_owner.id
          JOIN args a_track ON l.arg_set_id = a_track.arg_set_id AND a_track.key = 'track_id'
          JOIN cpu_counter_track t ON a_track.int_value = t.id
          WHERE l.name = 'machine_track_events_hidden' AND t.cpu = 0
          ORDER BY section, ts, trace_name;
        ''',
        out=Csv('''
        "section","cpu","ts","value","trace_name","duplicate_tracks"
        "counter",0,1000,1000000.000000,"[NULL]",0
        "counter",0,2000,1200000.000000,"[NULL]",0
        "counter",0,6000,1800000.000000,"[NULL]",0
        "import_log",0,1500,"[NULL]","trace2.pftrace owner=trace1.pftrace","[NULL]"
        "stat",0,"[NULL]",3,"trace2.pftrace","[NULL]"
        '''))

  # Meminfo members: shared MemFree kept from first owner; distinct members both kept.
  def test_machine_meminfo_members(self):
    t1 = """
        packet {
          timestamp: 1000
          sys_stats {
            meminfo { key: MEMINFO_MEM_FREE value: 100000 }
            meminfo { key: MEMINFO_CACHED value: 200000 }
          }
        }
        packet {
          timestamp: 2000
          sys_stats {
            meminfo { key: MEMINFO_MEM_FREE value: 110000 }
            meminfo { key: MEMINFO_CACHED value: 210000 }
          }
        }
        """
    t2 = """
        packet {
          timestamp: 1000
          sys_stats {
            meminfo { key: MEMINFO_MEM_FREE value: 150000 }
            meminfo { key: MEMINFO_BUFFERS value: 300000 }
          }
        }
        packet {
          timestamp: 2000
          sys_stats {
            meminfo { key: MEMINFO_MEM_FREE value: 160000 }
            meminfo { key: MEMINFO_BUFFERS value: 310000 }
          }
        }
        """
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query=_machine_query(
            track_type='meminfo',
            track_table='counter_track',
            track_filter="t.name IN ('MemFree', 'Cached', 'Buffers')"),
        out=Csv('''
        "section","ts","value","track_name","trace_name","duplicate_tracks"
        "counter",1000,307200000.000000,"Buffers","[NULL]",0
        "counter",2000,317440000.000000,"Buffers","[NULL]",0
        "counter",1000,204800000.000000,"Cached","[NULL]",0
        "counter",2000,215040000.000000,"Cached","[NULL]",0
        "counter",1000,102400000.000000,"MemFree","[NULL]",0
        "counter",2000,112640000.000000,"MemFree","[NULL]",0
        "import_log",1000,"[NULL]","MemFree","trace2.pftrace owner=trace1.pftrace","[NULL]"
        "stat","[NULL]",2,"MemFree","trace2.pftrace","[NULL]"
        '''))

  # Different formats: ftrace in A and sys_stats in B share the track; first writer owns.
  def test_machine_cpufreq_other_format(self):
    t1 = _ftrace(_cpu_freq(1000, 1000000), _cpu_freq(3000, 1200000))
    t2 = """
        packet {
          timestamp: 2000
          sys_stats { cpufreq_khz: 2000000 }
        }
        packet {
          timestamp: 4000
          sys_stats { cpufreq_khz: 4000000 }
        }
        """
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query=_machine_query(),
        out=Csv('''
        "section","ts","value","track_name","trace_name","duplicate_tracks"
        "counter",1000,1000000.000000,"cpufreq","[NULL]",0
        "counter",3000,1200000.000000,"cpufreq","[NULL]",0
        "counter",4000,4000000.000000,"cpufreq","[NULL]",0
        "import_log",2000,"[NULL]","cpufreq","trace2.pftrace owner=trace1.pftrace","[NULL]"
        "stat","[NULL]",1,"cpufreq","trace2.pftrace","[NULL]"
        '''))

  # Tie order: sorter tie-break gives drained queue priority at identical timestamps.
  def test_machine_tie_drained_queue_first(self):
    t1 = _ftrace(_cpu_freq(10, 1000000))
    t2 = """
        packet {
          ftrace_events {
            cpu: 0
            event { timestamp: 5 pid: 1 sched_switch { prev_comm: "t" prev_pid: 1 prev_state: 0 next_comm: "t" next_pid: 2 next_prio: 120 } }
            event { timestamp: 10 pid: 1 cpu_frequency { state: 2000000 cpu_id: 0 } }
          }
        }
        """
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query=_machine_query(),
        out=Csv('''
        "section","ts","value","track_name","trace_name","duplicate_tracks"
        "counter",10,2000000.000000,"cpufreq","[NULL]",0
        "import_log",10,"[NULL]","cpufreq","trace1.pftrace owner=trace2.pftrace","[NULL]"
        "stat","[NULL]",1,"cpufreq","trace1.pftrace","[NULL]"
        '''))

  # CPU-per-UID totals: series interpolates across handover with no step down.
  def test_machine_cpu_per_uid_handover(self):
    t1 = """
        packet {
          timestamp: 0
          cpu_per_uid_data {
            cluster_count: 1
            uid: 0
            total_time_ms: 1000
          }
        }
        packet {
          timestamp: 600000000000
          cpu_per_uid_data {
            uid: 0
            total_time_ms: 500
          }
        }
        packet {
          timestamp: 602000000000
          ftrace_events {
            cpu: 0
            event { timestamp: 602000000000 pid: 1 sched_switch { prev_comm: "t" prev_pid: 1 prev_state: 0 next_comm: "t" next_pid: 2 next_prio: 120 } }
          }
        }
        """
    t2 = """
        packet {
          timestamp: 0
          cpu_per_uid_data {
            cluster_count: 1
            uid: 0
            total_time_ms: 1000
          }
        }
        packet {
          timestamp: 595000000000
          cpu_per_uid_data {
            uid: 0
            total_time_ms: 200
          }
        }
        packet {
          timestamp: 601000000000
          cpu_per_uid_data {
            uid: 0
            total_time_ms: 36
          }
        }
        packet {
          timestamp: 605000000000
          cpu_per_uid_data {
            uid: 0
            total_time_ms: 24
          }
        }
        packet {
          timestamp: 610000000000
          cpu_per_uid_data {
            uid: 0
            total_time_ms: 40
          }
        }
        """
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query=_machine_query(
            track_type='android_cpu_per_uid_totals',
            track_table='counter_track',
            track_filter="t.name = 'CPU for System CL0'"),
        out=Csv('''
        "section","ts","value","track_name","trace_name","duplicate_tracks"
        "counter",0,0.000000,"CPU for System CL0","[NULL]",0
        "counter",600000000000,500.000000,"CPU for System CL0","[NULL]",0
        "counter",605000000000,530.000000,"CPU for System CL0","[NULL]",0
        "counter",610000000000,570.000000,"CPU for System CL0","[NULL]",0
        "import_log",0,"[NULL]","CPU for System CL0","trace2.pftrace owner=trace1.pftrace","[NULL]"
        "stat","[NULL]",3,"CPU for System CL0","trace2.pftrace","[NULL]"
        '''))

  # CPU-per-UID gap: first sample after gap falls back to previous owner's last value.
  def test_machine_cpu_per_uid_consecutive_gap(self):
    t1 = """
        packet {
          timestamp: 1000
          cpu_per_uid_data {
            cluster_count: 1
            uid: 0
            total_time_ms: 1000
          }
        }
        packet {
          timestamp: 2000
          cpu_per_uid_data {
            uid: 0
            total_time_ms: 500
          }
        }
        """
    t2 = """
        packet {
          timestamp: 3000
          cpu_per_uid_data {
            cluster_count: 1
            uid: 0
            total_time_ms: 2000
          }
        }
        packet {
          timestamp: 4000
          cpu_per_uid_data {
            uid: 0
            total_time_ms: 200
          }
        }
        """
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query=f'''
          SELECT
            t.name AS track_name,
            c.ts,
            c.value,
            (SELECT count(*) FROM (
               SELECT type, dimension_arg_set_id, machine_id
               FROM track
               WHERE type IN ({_MACHINE_SCOPED_TYPES_SQL})
               GROUP BY 1,2,3
               HAVING count(*) > 1
             )) AS duplicate_tracks,
            (SELECT coalesce(sum(value), 0) FROM stats WHERE name = 'machine_track_events_hidden') AS hidden_events
          FROM counter c
          JOIN counter_track t ON c.track_id = t.id
          WHERE t.type = 'android_cpu_per_uid_totals' AND t.name = 'CPU for System CL0'
          ORDER BY c.ts;
        ''',
        out=Csv('''
        "track_name","ts","value","duplicate_tracks","hidden_events"
        "CPU for System CL0",1000,0.000000,0,0
        "CPU for System CL0",2000,500.000000,0,0
        "CPU for System CL0",3000,500.000000,0,0
        "CPU for System CL0",4000,700.000000,0,0
        '''))

  # CPU-per-UID chain: totals accumulate monotonically across A -> B -> C.
  def test_machine_cpu_per_uid_chain(self):
    t1 = """
        packet {
          timestamp: 1000
          cpu_per_uid_data {
            cluster_count: 1
            uid: 0
            total_time_ms: 1000
          }
        }
        packet {
          timestamp: 2000
          cpu_per_uid_data {
            uid: 0
            total_time_ms: 100
          }
        }
        """
    t2 = """
        packet {
          timestamp: 1500
          cpu_per_uid_data {
            cluster_count: 1
            uid: 0
            total_time_ms: 2000
          }
        }
        packet {
          timestamp: 2500
          cpu_per_uid_data {
            uid: 0
            total_time_ms: 100
          }
        }
        packet {
          timestamp: 3500
          cpu_per_uid_data {
            uid: 0
            total_time_ms: 100
          }
        }
        """
    t3 = """
        packet {
          timestamp: 3000
          cpu_per_uid_data {
            cluster_count: 1
            uid: 0
            total_time_ms: 3000
          }
        }
        packet {
          timestamp: 4000
          cpu_per_uid_data {
            uid: 0
            total_time_ms: 100
          }
        }
        packet {
          timestamp: 5000
          cpu_per_uid_data {
            uid: 0
            total_time_ms: 100
          }
        }
        """
    return DiffTestBlueprint(
        trace=ZipTrace({
            'trace1.pftrace': TextProto(t1),
            'trace2.pftrace': TextProto(t2),
            'trace3.pftrace': TextProto(t3),
        }),
        query=_machine_query(
            track_type='android_cpu_per_uid_totals',
            track_table='counter_track',
            track_filter="t.name = 'CPU for System CL0'"),
        out=Csv('''
        "section","ts","value","track_name","trace_name","duplicate_tracks"
        "counter",1000,0.000000,"CPU for System CL0","[NULL]",0
        "counter",2000,100.000000,"CPU for System CL0","[NULL]",0
        "counter",2500,150.000000,"CPU for System CL0","[NULL]",0
        "counter",3500,250.000000,"CPU for System CL0","[NULL]",0
        "counter",4000,300.000000,"CPU for System CL0","[NULL]",0
        "counter",5000,400.000000,"CPU for System CL0","[NULL]",0
        "import_log",1500,"[NULL]","CPU for System CL0","trace2.pftrace owner=trace1.pftrace","[NULL]"
        "import_log",3000,"[NULL]","CPU for System CL0","trace3.pftrace owner=trace2.pftrace","[NULL]"
        "stat","[NULL]",1,"CPU for System CL0","trace2.pftrace","[NULL]"
        "stat","[NULL]",1,"CPU for System CL0","trace3.pftrace","[NULL]"
        '''))

  # GPU work period: negative duration inside owner's claim is neither kept nor hidden.
  def test_machine_gpu_work_period(self):
    snapshot = """
        packet {
          clock_snapshot {
            clocks { clock_id: 5 timestamp: 0 }
            clocks { clock_id: 6 timestamp: 0 }
          }
        }
        """
    t1 = snapshot + """
        packet {
          ftrace_events {
            cpu: 0
            event { timestamp: 1000 pid: 1 gpu_work_period { gpu_id: 0 uid: 1000 start_time_ns: 1000 end_time_ns: 2000 total_active_duration_ns: 500 } }
            event { timestamp: 5000 pid: 1 sched_switch { prev_comm: "t" prev_pid: 1 prev_state: 0 next_comm: "t" next_pid: 2 next_prio: 120 } }
          }
        }
        """
    t2 = snapshot + """
        packet {
          ftrace_events {
            cpu: 0
            event { timestamp: 1000 pid: 1 gpu_work_period { gpu_id: 0 uid: 1000 start_time_ns: 1000 end_time_ns: 2000 total_active_duration_ns: 500 } }
            event { timestamp: 4000 pid: 1 gpu_work_period { gpu_id: 0 uid: 1000 start_time_ns: 4000 end_time_ns: 6000 total_active_duration_ns: 1000 } }
            event { timestamp: 4500 pid: 1 gpu_work_period { gpu_id: 0 uid: 1000 start_time_ns: 4500 end_time_ns: 4400 total_active_duration_ns: 0 } }
            event { timestamp: 6000 pid: 1 gpu_work_period { gpu_id: 0 uid: 1000 start_time_ns: 6000 end_time_ns: 7000 total_active_duration_ns: 500 } }
          }
        }
        """
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query=f'''
          SELECT
            'slice' AS section,
            s.ts,
            s.dur,
            s.name AS name,
            t.type AS track_type,
            NULL AS trace_name,
            NULL AS value,
            (SELECT count(*) FROM (
               SELECT type, dimension_arg_set_id, machine_id
               FROM track
               WHERE type IN ({_MACHINE_SCOPED_TYPES_SQL})
               GROUP BY 1,2,3
               HAVING count(*) > 1
             )) AS duplicate_tracks
          FROM slice s
          JOIN track t ON s.track_id = t.id
          WHERE t.type = 'android_gpu_work_period'

          UNION ALL

          SELECT
            'stat' AS section,
            NULL AS ts,
            NULL AS dur,
            s.name AS name,
            t.type AS track_type,
            tf.name AS trace_name,
            s.value,
            NULL AS duplicate_tracks
          FROM stats s
          JOIN __intrinsic_trace_file tf ON s.trace_id = tf.id
          LEFT JOIN track t ON s.idx = t.id
          WHERE s.name IN ('machine_track_events_hidden', 'gpu_work_period_negative_duration')
            AND s.value > 0

          UNION ALL

          SELECT
            'import_log' AS section,
            l.ts,
            NULL AS dur,
            l.name AS name,
            t.type AS track_type,
            CASE
              WHEN tf_owner.name IS NOT NULL THEN printf('%s owner=%s', tf_writer.name, tf_owner.name)
              ELSE tf_writer.name
            END AS trace_name,
            NULL AS value,
            NULL AS duplicate_tracks
          FROM _trace_import_logs l
          JOIN __intrinsic_trace_file tf_writer ON l.trace_id = tf_writer.id
          LEFT JOIN args a_owner ON l.arg_set_id = a_owner.arg_set_id AND a_owner.key = 'owner_trace_id'
          LEFT JOIN __intrinsic_trace_file tf_owner ON a_owner.int_value = tf_owner.id
          LEFT JOIN args a_track ON l.arg_set_id = a_track.arg_set_id AND a_track.key = 'track_id'
          LEFT JOIN track t ON a_track.int_value = t.id
          WHERE l.name IN ('machine_track_events_hidden', 'gpu_work_period_negative_duration')
          ORDER BY section, ts, name, trace_name;
        ''',
        out=Csv('''
        "section","ts","dur","name","track_type","trace_name","value","duplicate_tracks"
        "import_log",1000,"[NULL]","machine_track_events_hidden","android_gpu_work_period","trace2.pftrace owner=trace1.pftrace","[NULL]","[NULL]"
        "import_log",4500,"[NULL]","gpu_work_period_negative_duration","[NULL]","trace2.pftrace","[NULL]","[NULL]"
        "slice",1000,1000,"%50.00","android_gpu_work_period","[NULL]","[NULL]",0
        "slice",6000,1000,"%50.00","android_gpu_work_period","[NULL]","[NULL]",0
        "stat","[NULL]","[NULL]","gpu_work_period_negative_duration","[NULL]","trace2.pftrace",1,"[NULL]"
        "stat","[NULL]","[NULL]","machine_track_events_hidden","android_gpu_work_period","trace2.pftrace",2,"[NULL]"
        '''))

  # Kernel wakelocks: overlapping traces share sparse track; B hidden during A's claim, then takes over.
  def test_machine_kernel_wakelocks(self):
    t1 = """
        packet {
          timestamp: 1000
          trusted_uid: 9999
          trusted_packet_sequence_id: 1
          sequence_flags: 3
          previous_packet_dropped: 1
          first_packet_on_sequence: true
          kernel_wakelock_data {
            wakelock {
              wakelock_id: 1
              wakelock_name: "wakelock_a"
              wakelock_type: WAKELOCK_TYPE_KERNEL
            }
            wakelock_id: 1
            time_held_millis: 10
          }
        }
        packet {
          timestamp: 3000
          trusted_uid: 9999
          trusted_packet_sequence_id: 1
          sequence_flags: 2
          kernel_wakelock_data {
            wakelock_id: 1
            time_held_millis: 5
          }
        }
        """
    t2 = """
        packet {
          timestamp: 2000
          trusted_uid: 9999
          trusted_packet_sequence_id: 2
          sequence_flags: 3
          previous_packet_dropped: 1
          first_packet_on_sequence: true
          kernel_wakelock_data {
            wakelock {
              wakelock_id: 1
              wakelock_name: "wakelock_a"
              wakelock_type: WAKELOCK_TYPE_KERNEL
            }
            wakelock_id: 1
            time_held_millis: 20
          }
        }
        packet {
          timestamp: 4000
          trusted_uid: 9999
          trusted_packet_sequence_id: 2
          sequence_flags: 2
          kernel_wakelock_data {
            wakelock_id: 1
            time_held_millis: 10
          }
        }
        packet {
          timestamp: 5000
          trusted_uid: 9999
          trusted_packet_sequence_id: 2
          sequence_flags: 2
          kernel_wakelock_data {
            wakelock_id: 1
            time_held_millis: 10
          }
        }
        """
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query=_machine_query(
            track_type='android_kernel_wakelock',
            track_table='counter_track',
        ),
        out=Csv('''
        "section","ts","value","track_name","trace_name","duplicate_tracks"
        "counter",1000,10000000.000000,"wakelock_a","[NULL]",0
        "counter",3000,15000000.000000,"wakelock_a","[NULL]",0
        "counter",4000,30000000.000000,"wakelock_a","[NULL]",0
        "counter",5000,40000000.000000,"wakelock_a","[NULL]",0
        "import_log",2000,"[NULL]","wakelock_a","trace2.pftrace owner=trace1.pftrace","[NULL]"
        "stat","[NULL]",1,"wakelock_a","trace2.pftrace","[NULL]"
        '''))

  # Bugreport battery history: dumpstate history claims track first; proto trace hidden until history ends.
  def test_machine_bugreport_battery_history(self):
    dumpstate = """========================================================
== dumpstate: 2022-06-24 16:24:37
========================================================
------ CHECKIN BATTERYSTATS (dumpsys batterystats -c) ------
9,0,i,vers,36,180,P,1
9,h,0:TIME:1656087877000
9,h,0,Bl=85,Bs=d,+S
9,h,2000,Bl=83
------ 0.001s was the duration of 'CHECKIN BATTERYSTATS' ------
"""
    proto = """
        packet {
          timestamp: 1656087878000000000
          battery {
            capacity_percent: 84
          }
        }
        packet {
          timestamp: 1656087880000000000
          battery {
            capacity_percent: 80
          }
        }
        """
    return DiffTestBlueprint(
        trace=ZipTrace({
            'bugreport.zip': _make_bugreport_zip(dumpstate),
            'trace.pftrace': TextProto(proto),
        }),
        query=_machine_query(
            track_type='battery_counter',
            track_table='counter_track',
        ),
        out=Csv('''
        "section","ts","value","track_name","trace_name","duplicate_tracks"
        "counter",1656087877000000000,85.000000,"batt.capacity_pct","[NULL]",0
        "counter",1656087879000000000,83.000000,"batt.capacity_pct","[NULL]",0
        "counter",1656087880000000000,80.000000,"batt.capacity_pct","[NULL]",0
        "import_log",1656087878000000000,"[NULL]","batt.capacity_pct","trace.pftrace owner=","[NULL]"
        "stat","[NULL]",1,"batt.capacity_pct","trace.pftrace","[NULL]"
        '''))

  # Guard: power_rails, diskstat, and net counters remain per trace.
  def test_machine_not_machine_scoped_types(self):
    t1 = """
        packet {
          power_rails {
            rail_descriptor { index: 1 rail_name: "rail1" subsys_name: "subsys1" sampling_rate: 1000 }
            energy_data { index: 1 timestamp_ms: 1 energy: 100 }
          }
        }
        packet {
          timestamp: 1000
          sys_stats {
            disk_stat { device_name: "sda" read_sectors: 10 write_sectors: 20 }
          }
        }
        packet {
          timestamp: 1100
          sys_stats {
            disk_stat { device_name: "sda" read_sectors: 15 write_sectors: 25 }
          }
        }
        packet {
          ftrace_events {
            cpu: 0
            event { timestamp: 1000 pid: 1 net_dev_xmit { name: "wlan0" len: 100 rc: 0 } }
          }
        }
        """
    t2 = """
        packet {
          power_rails {
            rail_descriptor { index: 1 rail_name: "rail1" subsys_name: "subsys1" sampling_rate: 1000 }
            energy_data { index: 1 timestamp_ms: 2 energy: 200 }
          }
        }
        packet {
          timestamp: 2000
          sys_stats {
            disk_stat { device_name: "sda" read_sectors: 30 write_sectors: 40 }
          }
        }
        packet {
          timestamp: 2100
          sys_stats {
            disk_stat { device_name: "sda" read_sectors: 35 write_sectors: 45 }
          }
        }
        packet {
          ftrace_events {
            cpu: 0
            event { timestamp: 2000 pid: 1 net_dev_xmit { name: "wlan0" len: 200 rc: 0 } }
          }
        }
        """
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query='''
          SELECT
            t.type,
            count(DISTINCT t.id) AS track_count,
            (SELECT coalesce(sum(value), 0) FROM stats WHERE name = 'machine_track_events_hidden') AS hidden_events
          FROM track t
          WHERE t.type IN ('power_rails', 'diskstat', 'net_transmit')
          GROUP BY t.type
          ORDER BY t.type;
        ''',
        out=Csv('''
        "type","track_count","hidden_events"
        "diskstat",16,0
        "net_transmit",2,0
        "power_rails",2,0
        '''))

  # Guard: trace plus logcat produces unchanged output and zero hidden-event stats.
  def test_machine_trace_and_logcat_no_stats(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            'trace.pftrace':
                TextProto(
                    _ftrace(_cpu_freq(1000, 1000000), _cpu_freq(2000,
                                                                1200000))),
            'logcat.txt':
                '06-24 16:32:00.000 1000 1000 I tag: hello\n',
        }),
        query=f'''
          SELECT
            c.ts,
            c.value,
            (SELECT count(*) FROM (
               SELECT type, dimension_arg_set_id, machine_id
               FROM track
               WHERE type IN ({_MACHINE_SCOPED_TYPES_SQL})
               GROUP BY 1,2,3
               HAVING count(*) > 1
             )) AS duplicate_tracks,
            (SELECT coalesce(sum(value), 0) FROM stats WHERE name = 'machine_track_events_hidden') AS hidden_events
          FROM counter c
          JOIN cpu_counter_track t ON c.track_id = t.id
          WHERE t.type = 'cpu_frequency'
          ORDER BY c.ts;
        ''',
        out=Csv('''
        "ts","value","duplicate_tracks","hidden_events"
        1000,1000000.000000,0,0
        2000,1200000.000000,0,0
        '''))

  # Guard: single trace in zip is unchanged and produces zero hidden-event stats.
  def test_machine_single_trace_unchanged(self):
    return DiffTestBlueprint(
        trace=ZipTrace({
            'trace.pftrace':
                TextProto(
                    _ftrace(_cpu_freq(1000, 1000000), _cpu_freq(2000,
                                                                1200000))),
        }),
        query=f'''
          SELECT
            c.ts,
            c.value,
            (SELECT count(*) FROM (
               SELECT type, dimension_arg_set_id, machine_id
               FROM track
               WHERE type IN ({_MACHINE_SCOPED_TYPES_SQL})
               GROUP BY 1,2,3
               HAVING count(*) > 1
             )) AS duplicate_tracks,
            (SELECT coalesce(sum(value), 0) FROM stats WHERE name = 'machine_track_events_hidden') AS hidden_events
          FROM counter c
          JOIN cpu_counter_track t ON c.track_id = t.id
          WHERE t.type = 'cpu_frequency'
          ORDER BY c.ts;
        ''',
        out=Csv('''
        "ts","value","duplicate_tracks","hidden_events"
        1000,1000000.000000,0,0
        2000,1200000.000000,0,0
        '''))

  # Guard: separate machines keep separate tracks with zero hidden-event stats.
  def test_machine_different_machines(self):
    payload = _ftrace(_cpu_freq(1000, 1000000))
    manifest = json.dumps({
        'perfetto_manifest': {
            'version':
                1,
            'files': [
                {
                    'path': 'trace1.pftrace',
                    'machine': {
                        'name': 'm1'
                    }
                },
                {
                    'path': 'trace2.pftrace',
                    'machine': {
                        'name': 'm2'
                    }
                },
            ],
        }
    })
    return DiffTestBlueprint(
        trace=ZipTrace({
            'meta.json': manifest,
            'trace1.pftrace': TextProto(payload),
            'trace2.pftrace': TextProto(payload),
        }),
        query=f'''
          SELECT
            m.name AS machine_name,
            c.ts,
            c.value,
            (SELECT count(*) FROM (
               SELECT type, dimension_arg_set_id, machine_id
               FROM track
               WHERE type IN ({_MACHINE_SCOPED_TYPES_SQL})
               GROUP BY 1,2,3
               HAVING count(*) > 1
             )) AS duplicate_tracks,
            (SELECT coalesce(sum(value), 0) FROM stats WHERE name = 'machine_track_events_hidden') AS hidden_events
          FROM counter c
          JOIN cpu_counter_track t ON c.track_id = t.id
          JOIN machine m ON t.machine_id = m.id
          WHERE t.type = 'cpu_frequency'
          ORDER BY machine_name, c.ts;
        ''',
        out=Csv('''
        "machine_name","ts","value","duplicate_tracks","hidden_events"
        "m1",1000,1000000.000000,0,0
        "m2",1000,1000000.000000,0,0
        '''))

  # Multi-type trace merge: no duplicate tracks across all machine-scoped types.
  def test_machine_no_duplicate_tracks(self):
    snapshot = """
        packet {
          clock_snapshot {
            clocks { clock_id: 5 timestamp: 0 }
            clocks { clock_id: 6 timestamp: 0 }
          }
        }
        """
    t1 = snapshot + """
        packet {
          ftrace_events {
            cpu: 0
            event { timestamp: 1000 pid: 1 cpu_frequency { state: 1000 cpu_id: 0 } }
            event { timestamp: 1000 pid: 1 gpu_work_period { gpu_id: 0 uid: 1000 start_time_ns: 1000 end_time_ns: 2000 total_active_duration_ns: 500 } }
          }
        }
        packet {
          timestamp: 1000
          generic_gpu_frequency_event { gpu_id: 0 frequency_khz: 1500000 }
        }
        packet {
          timestamp: 1000
          entity_state_residency {
            power_entity_state { entity_index: 0 state_index: 0 entity_name: "BT" state_name: "Idle" }
            residency { entity_index: 0 state_index: 0 total_time_in_state_ms: 100 }
          }
        }
        packet {
          timestamp: 1000
          cpu_per_uid_data { cluster_count: 1 uid: 0 total_time_ms: 1000 }
        }
        """
    t2 = snapshot + """
        packet {
          ftrace_events {
            cpu: 0
            event { timestamp: 2000 pid: 1 cpu_frequency { state: 1200000 cpu_id: 0 } }
            event { timestamp: 2000 pid: 1 gpu_work_period { gpu_id: 0 uid: 1000 start_time_ns: 2000 end_time_ns: 3000 total_active_duration_ns: 500 } }
          }
        }
        packet {
          timestamp: 2000
          generic_gpu_frequency_event { gpu_id: 0 frequency_khz: 1800000 }
        }
        packet {
          timestamp: 2000
          entity_state_residency {
            power_entity_state { entity_index: 0 state_index: 0 entity_name: "BT" state_name: "Idle" }
            residency { entity_index: 0 state_index: 0 total_time_in_state_ms: 200 }
          }
        }
        packet {
          timestamp: 2000
          cpu_per_uid_data { cluster_count: 1 uid: 0 total_time_ms: 200 }
        }
        """
    return DiffTestBlueprint(
        trace=_two_trace_zip(t1, t2),
        query=f'''
          SELECT
            t.type,
            count(*) AS track_count,
            (SELECT count(*) FROM (
               SELECT type, dimension_arg_set_id, machine_id
               FROM track
               WHERE type IN ({_MACHINE_SCOPED_TYPES_SQL})
               GROUP BY 1,2,3
               HAVING count(*) > 1
             )) AS duplicate_tracks
          FROM track t
          WHERE t.type IN (
            'cpu_frequency',
            'gpu_frequency',
            'android_gpu_work_period',
            'entity_state',
            'android_cpu_per_uid_totals'
          )
          GROUP BY t.type
          ORDER BY t.type;
        ''',
        out=Csv('''
        "type","track_count","duplicate_tracks"
        "android_cpu_per_uid_totals",1,0
        "android_gpu_work_period",1,0
        "cpu_frequency",1,0
        "entity_state",1,0
        "gpu_frequency",1,0
        '''))
