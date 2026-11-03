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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite

# One sample per heap profile allocation: stacks [f1, f2, f3] and [f1, f2].
_SAMPLES = '(SELECT callsite_id FROM heap_profile_allocation WHERE count > 0)'


class Callstacks(TestSuite):

  def test_callsite_sample_counts(self):
    return DiffTestBlueprint(
        trace=Path('annotate.textproto'),
        query=f"""
        INCLUDE PERFETTO MODULE callstacks.annotate;

        SELECT f.name, c.self_count, c.total_count
        FROM _callsite_sample_counts!({_SAMPLES}) c
        JOIN stack_profile_callsite sc ON sc.id = c.callsite_id
        JOIN stack_profile_frame f ON f.id = sc.frame_id
        ORDER BY f.name;
        """,
        out=Csv("""
        "name","self_count","total_count"
        "f1",0,2
        "f2",1,2
        "f3",1,1
        """))

  def test_sample_counts_by_address(self):
    return DiffTestBlueprint(
        trace=Path('annotate.textproto'),
        query=f"""
        INCLUDE PERFETTO MODULE callstacks.annotate;

        SELECT mapping_name, build_id, rel_pc, self_count, total_count
        FROM _sample_counts_by_address!({_SAMPLES})
        ORDER BY rel_pc;
        """,
        out=Csv("""
        "mapping_name","build_id","rel_pc","self_count","total_count"
        "/liblib.so","6275696c642d6964",4096,0,2
        "/liblib.so","6275696c642d6964",8193,1,2
        "/liblib.so","6275696c642d6964",12288,1,1
        """))

  def test_sample_counts_by_source_line(self):
    return DiffTestBlueprint(
        trace=Path('annotate.textproto'),
        query=f"""
        INCLUDE PERFETTO MODULE callstacks.annotate;

        SELECT source_file, line_number, self_count, total_count
        FROM _sample_counts_by_source_line!({_SAMPLES})
        ORDER BY source_file;
        """,
        out=Csv("""
        "source_file","line_number","self_count","total_count"
        "f1.cc",1,0,2
        "f2.cc",2,1,2
        "f3.cc",33,1,1
        """))

  def test_disassembly_function_for_address(self):
    return DiffTestBlueprint(
        trace=Path('annotate.textproto'),
        query="""
        INCLUDE PERFETTO MODULE callstacks.annotate;

        SELECT
          f.rel_pc,
          (
            SELECT name FROM disassembly_function
            WHERE id = _disassembly_function_for_address(f.mapping, f.rel_pc)
          ) AS function_name
        FROM stack_profile_frame f
        ORDER BY f.rel_pc;
        """,
        out=Csv("""
        "rel_pc","function_name"
        4096,"[NULL]"
        8193,"f2"
        12288,"[NULL]"
        """))

  def test_annotated_disassembly(self):
    return DiffTestBlueprint(
        trace=Path('annotate.textproto'),
        query=f"""
        INCLUDE PERFETTO MODULE callstacks.annotate;

        SELECT
          rel_pc, text, target_rel_pc, target_symbol, source_file,
          line_number, self_count, total_count
        FROM _annotated_disassembly!(
          {_SAMPLES},
          (SELECT id FROM disassembly_function WHERE name = 'f2')
        );
        """,
        out=Csv("""
        "rel_pc","text","target_rel_pc","target_symbol","source_file","line_number","self_count","total_count"
        8192,"push rbp","[NULL]","[NULL]","f2.cc",1,0,0
        8193,"call 0x3000",12288,"f3","f2.cc",2,1,2
        8198,"jmp 0x2000",8192,"[NULL]","f2.cc",3,0,0
        """))

  def test_callstack_forest_rel_pc(self):
    return DiffTestBlueprint(
        trace=Path('annotate.textproto'),
        query=f"""
        INCLUDE PERFETTO MODULE callstacks.stack_profile;

        SELECT name, mapping_name, rel_pc, self_count
        FROM _callstacks_for_callsites!({_SAMPLES})
        ORDER BY rel_pc;
        """,
        out=Csv("""
        "name","mapping_name","rel_pc","self_count"
        "f1","/liblib.so",4096,0
        "f2","/liblib.so",8193,1
        "f3","/liblib.so",12288,1
        """))
