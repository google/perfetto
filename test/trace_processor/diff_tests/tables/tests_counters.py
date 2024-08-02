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

  def test_counter_dur_example_android_trace_30s(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
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

  def test_counter_dur_example_android_trace_30s_machine_id(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        trace_modifier=TraceInjector(
            ['ftrace_events', 'sys_stats', 'process_stats', 'process_tree'],
            {'machine_id': 1001}),
        query="""
        SELECT ts, dur, m.raw_id as raw_machine_id
        FROM experimental_counter_dur c
        JOIN counter_track t on c.track_id = t.id
        JOIN machine m on t.machine_id = m.id
        WHERE track_id IN (1, 2, 3)
        ORDER BY dur LIMIT 10;
        """,
        out=Csv("""
        "ts","dur","raw_machine_id"
        100351738640,-1,1001
        100351738640,-1,1001
        100351738640,-1,1001
        70731059648,19510835,1001
        70731059648,19510835,1001
        70731059648,19510835,1001
        73727335051,23522762,1001
        73727335051,23522762,1001
        73727335051,23522762,1001
        86726132752,24487554,1001
        """))

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
        JOIN cpu ON t.ucpu = cpu.id
        WHERE cpu.cpu = 1;
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
