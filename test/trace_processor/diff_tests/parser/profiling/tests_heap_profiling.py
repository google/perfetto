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
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ProfilingHeapProfiling(TestSuite):

  def test_heap_profile_jit(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_jit.textproto'),
        query="""
        SELECT name, mapping, rel_pc FROM stack_profile_frame ORDER BY name;
        """,
        out=Csv("""
        "name","mapping","rel_pc"
        "java_frame_1",0,4096
        "java_frame_2",0,4096
        """))

  def test_heap_profile_deobfuscate(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_deobfuscate.textproto'),
        query=Path('heap_profile_deobfuscate_test.sql'),
        out=Csv("""
        "deobfuscated_name","mapping","rel_pc"
        "Bar.function1",0,4096
        """))

  def test_heap_profile_deobfuscate_2(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_deobfuscate_memfd.textproto'),
        query=Path('heap_profile_deobfuscate_test.sql'),
        out=Csv("""
        "deobfuscated_name","mapping","rel_pc"
        "Bar.function1",0,4096
        """))

  def test_heap_profile_dump_max_legacy(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_dump_max_legacy.textproto'),
        query="""
        SELECT id, ts, upid, heap_name, callsite_id, count, size
        FROM heap_profile_allocation;
        """,
        out=Csv("""
        "id","ts","upid","heap_name","callsite_id","count","size"
        0,-10,2,"unknown",2,0,1000
        1,-10,2,"unknown",3,0,90
        """))

  def test_heap_profile_dump_max(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_dump_max.textproto'),
        query="""
        SELECT id, ts, upid, heap_name, callsite_id, count, size
        FROM heap_profile_allocation;
        """,
        out=Csv("""
        "id","ts","upid","heap_name","callsite_id","count","size"
        0,-10,2,"unknown",2,6,1000
        1,-10,2,"unknown",3,1,90
        """))

  # A dump with start_timestamp populates heap_profile, and allocations join it
  # via (upid, ts = ts_end).
  def test_heap_profile_window(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_window.textproto'),
        query="""
        SELECT hp.ts, hp.ts_end, hp.dur, a.ts AS alloc_ts, a.size
        FROM heap_profile AS hp
        JOIN heap_profile_allocation AS a
          ON hp.upid = a.upid AND a.ts = hp.ts_end;
        """,
        out=Csv("""
        "ts","ts_end","dur","alloc_ts","size"
        5,20,15,20,1000
        """))

  # Without start_timestamp the window collapses to a point at the dump time.
  def test_heap_profile_window_legacy(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_dump_max.textproto'),
        query="""
        SELECT ts, ts_end, dur FROM heap_profile;
        """,
        out=Csv("""
        "ts","ts_end","dur"
        -10,-10,0
        """))

  # With start_timestamp, the interval comes straight from heap_profile.
  def test_heap_profile_intervals(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_window.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.memory.heap_profile.intervals;
        SELECT heap_name, ts, dur FROM _android_heap_profile_intervals;
        """,
        out=Csv("""
        "heap_name","ts","dur"
        "unknown",5,15
        """))

  # Without start_timestamp, a dump's interval falls back to the previous dump
  # of the same heap, so continuous dumps still render as non-zero intervals.
  def test_heap_profile_intervals_legacy(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_continuous_legacy.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.memory.heap_profile.intervals;
        SELECT heap_name, ts, dur, ts + dur AS ts_end
        FROM _android_heap_profile_intervals
        ORDER BY ts_end;
        """,
        out=Csv("""
        "heap_name","ts","dur","ts_end"
        "unknown",20,0,20
        "unknown",21,19,40
        """))
