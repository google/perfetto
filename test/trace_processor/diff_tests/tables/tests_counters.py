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
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint, TraceInjector
from python.generators.diff_tests.testing import TestSuite


class TablesCounters(TestSuite):
  # Counters table
  def test_synth_1_filter_counter(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query="""
        SELECT COUNT(*)
        FROM counter
        WHERE
          track_id = 0;
        """,
        out=Csv("""
        "COUNT(*)"
        2
        """))

  def test_memory_counters_b120278869_neg_ts_end(self):
    return DiffTestBlueprint(
        trace=DataPath('memory_counters.pb'),
        query="""
        SELECT count(*) FROM counters WHERE -1 < ts;
        """,
        out=Csv("""
        "count(*)"
        98688
        """))

  def test_counters_where_cpu_counters_where_cpu(self):
    return DiffTestBlueprint(
        trace=Path('counters_where_cpu.py'),
        query="""
        SELECT
          ts,
          lead(ts, 1, ts) OVER (PARTITION BY name ORDER BY ts) - ts AS dur,
          value
        FROM counter c
        JOIN cpu_counter_track t ON t.id = c.track_id
        WHERE cpu = 1;
        """,
        out=Csv("""
        "ts","dur","value"
        1000,1,3000.000000
        1001,0,4000.000000
        """))

  def test_counters_group_by_freq_counters_group_by_freq(self):
    return DiffTestBlueprint(
        trace=Path('counters_group_by_freq.py'),
        query="""
        SELECT
          value,
          sum(dur) AS dur_sum
        FROM (
          SELECT value,
            lead(ts) OVER (PARTITION BY name, track_id ORDER BY ts) - ts AS dur
          FROM counter
          JOIN counter_track ON counter.track_id = counter_track.id
        )
        WHERE value > 0
        GROUP BY value
        ORDER BY dur_sum DESC;
        """,
        out=Csv("""
        "value","dur_sum"
        4000.000000,2
        3000.000000,1
        """))

  def test_filter_row_vector_example_android_trace_30s(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        SELECT ts
        FROM counter
        WHERE
          ts > 72563651549
          AND track_id = (
            SELECT t.id
            FROM process_counter_track t
            JOIN process p USING (upid)
            WHERE
              t.name = 'Heap size (KB)'
              AND p.pid = 1204
          )
          AND value != 17952.000000
        LIMIT 20;
        """,
        out=Path('filter_row_vector_example_android_trace_30s.out'))

  # Tests counter.machine_id and process_counter_track.machine.
  def test_filter_row_vector_example_android_trace_30s_machine_id(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        trace_modifier=TraceInjector(
            ['ftrace_events', 'sys_stats', 'process_stats', 'process_tree'],
            {'machine_id': 1001}),
        query="""
        SELECT ts
        FROM counter
        WHERE
          ts > 72563651549
          AND track_id = (
            SELECT t.id
            FROM process_counter_track t
            JOIN process p USING (upid)
            WHERE
              t.name = 'Heap size (KB)'
              AND p.pid = 1204
              AND t.machine_id is not NULL
          )
          AND value != 17952.000000
        LIMIT 20;
        """,
        out=Path('filter_row_vector_example_android_trace_30s.out'))

  def test_counters_where_cpu_counters_where_cpu_machine_id(self):
    return DiffTestBlueprint(
        trace=Path('counters_where_cpu.py'),
        trace_modifier=TraceInjector(['ftrace_events'], {'machine_id': 1001}),
        query="""
        SELECT
          ts,
          lead(ts, 1, ts) OVER (PARTITION BY track_id ORDER BY ts) - ts AS dur,
          value
        FROM counter c
        JOIN cpu_counter_track t ON c.track_id = t.id
        WHERE t.cpu = 1;
        """,
        out=Csv("""
        "ts","dur","value"
        1000,1,3000.000000
        1001,0,4000.000000
        """))

  def test_synth_1_filter_counter_machine_id(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        trace_modifier=TraceInjector(
            ['ftrace_events', 'process_stats', 'process_tree'],
            {'machine_id': 1001}),
        query="""
        SELECT COUNT(*)
        FROM counter
        WHERE
          track_id = 0;
        """,
        out=Csv("""
        "COUNT(*)"
        2
        """))

  def test_memory_counters_machine_id(self):
    return DiffTestBlueprint(
        trace=DataPath('memory_counters.pb'),
        trace_modifier=TraceInjector(
            ['ftrace_events', 'sys_stats', 'process_stats', 'process_tree'],
            {'machine_id': 1001}),
        query="""
        SELECT count(*)
        FROM counter
        JOIN counter_track on counter_track.id = counter.track_id
        WHERE -1 < ts group by machine_id;
        """,
        out=Csv("""
        "count(*)"
        98688
        """))

  def test_counters_utid_arg_set_id(self):
    return DiffTestBlueprint(
        trace=DataPath('memory_counters.pb'),
        trace_modifier=TraceInjector(
            ['ftrace_events', 'sys_stats', 'process_stats', 'process_tree'],
            {'machine_id': 1001}),
        query="""
        SELECT COUNT(DISTINCT extract_arg(arg_set_id, 'utid')) AS utid_count FROM counter
        """,
        out=Csv("""
        "utid_count"
        141
        """))

  def test_cpu_counter_track_args_multi_machine(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 2244000533469
          sys_stats {
            cpu_stat {
              cpu_id: 0
              user_ns: 119650000000
            }
            cpu_stat {
              cpu_id: 1
              user_ns: 88530000000
            }
          }
          trusted_packet_sequence_id: 1
        }
        packet {
          timestamp: 22440005334670
          sys_stats {
            cpu_stat {
              cpu_id: 0
              user_ns: 119650000000
            }
            cpu_stat {
              cpu_id: 1
              user_ns: 88530000000
            }
          }
          trusted_packet_sequence_id: 1
          machine_id: 1001
        }
        """),
        query="""
        SELECT id, arg_set_id
        FROM args
        WHERE flat_key="cpustat_key" AND display_value="user_ns"
        """,
        out=Csv("""
        "id","arg_set_id"
        1,0
        17,16
        """))

  def test_cpu_counter_track_multi_machine(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 2244000533469
          sys_stats {
            cpu_stat {
              cpu_id: 0
              user_ns: 119650000000
            }
            cpu_stat {
              cpu_id: 1
              user_ns: 88530000000
            }
          }
          trusted_packet_sequence_id: 1
        }
        packet {
          timestamp: 22440005334670
          sys_stats {
            cpu_stat {
              cpu_id: 0
              user_ns: 119650000000
            }
            cpu_stat {
              cpu_id: 1
              user_ns: 88530000000
            }
          }
          trusted_packet_sequence_id: 1
          machine_id: 1001
        }
        """),
        query="""
        SELECT id, name, machine_id, cpu
        FROM cpu_counter_track;
        """,
        out=Csv("""
        "id","name","machine_id","cpu"
        0,"cpu.times.user_ns","[NULL]",0
        1,"cpu.times.user_nice_ns","[NULL]",0
        2,"cpu.times.system_mode_ns","[NULL]",0
        3,"cpu.times.idle_ns","[NULL]",0
        4,"cpu.times.io_wait_ns","[NULL]",0
        5,"cpu.times.irq_ns","[NULL]",0
        6,"cpu.times.softirq_ns","[NULL]",0
        7,"cpu.times.steal_ns","[NULL]",0
        8,"cpu.times.user_ns","[NULL]",1
        9,"cpu.times.user_nice_ns","[NULL]",1
        10,"cpu.times.system_mode_ns","[NULL]",1
        11,"cpu.times.idle_ns","[NULL]",1
        12,"cpu.times.io_wait_ns","[NULL]",1
        13,"cpu.times.irq_ns","[NULL]",1
        14,"cpu.times.softirq_ns","[NULL]",1
        15,"cpu.times.steal_ns","[NULL]",1
        16,"cpu.times.user_ns",1,0
        17,"cpu.times.user_nice_ns",1,0
        18,"cpu.times.system_mode_ns",1,0
        19,"cpu.times.idle_ns",1,0
        20,"cpu.times.io_wait_ns",1,0
        21,"cpu.times.irq_ns",1,0
        22,"cpu.times.softirq_ns",1,0
        23,"cpu.times.steal_ns",1,0
        24,"cpu.times.user_ns",1,1
        25,"cpu.times.user_nice_ns",1,1
        26,"cpu.times.system_mode_ns",1,1
        27,"cpu.times.idle_ns",1,1
        28,"cpu.times.io_wait_ns",1,1
        29,"cpu.times.irq_ns",1,1
        30,"cpu.times.softirq_ns",1,1
        31,"cpu.times.steal_ns",1,1
        """))
