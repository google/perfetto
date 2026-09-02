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


class ProfilingSourceAndDisassembly(TestSuite):

  def test_source_file(self):
    return DiffTestBlueprint(
        trace=Path('source_and_disassembly.textproto'),
        query="""
        SELECT path, contents FROM source_file;
        """,
        out=Csv("""
        "path","contents"
        "f2.cc","int f2() {
          return 2;
        }
        "
        """))

  def test_disassembly_function(self):
    return DiffTestBlueprint(
        trace=Path('source_and_disassembly.textproto'),
        query="""
        SELECT path, build_id, name, start_rel_pc, size
        FROM disassembly_function;
        """,
        out=Csv("""
        "path","build_id","name","start_rel_pc","size"
        "/liblib.so","6275696c642d6964","f2",8192,8
        """))

  def test_disassembly_instruction(self):
    return DiffTestBlueprint(
        trace=Path('source_and_disassembly.textproto'),
        query="""
        SELECT
          f.name AS function_name,
          i.rel_pc,
          i.bytes,
          i.text,
          i.target_rel_pc,
          i.target_symbol,
          i.source_file,
          i.line_number
        FROM disassembly_instruction i
        JOIN disassembly_function f ON i.function_id = f.id
        ORDER BY i.rel_pc;
        """,
        out=Csv("""
        "function_name","rel_pc","bytes","text","target_rel_pc","target_symbol","source_file","line_number"
        "f2",8192,"55","push rbp","[NULL]","[NULL]","f2.cc",1
        "f2",8193,"ebfd","jmp 0x2000",8192,"[NULL]","f2.cc",2
        "f2",8195,"e800000000","call 0x3000",12288,"f3","[NULL]","[NULL]"
        """))

  def test_disassembly_unknown_module_is_dropped(self):
    return DiffTestBlueprint(
        trace=Path('source_and_disassembly.textproto'),
        query="""
        SELECT value FROM stats WHERE name = 'disassembly_invalid_mapping_id';
        """,
        out=Csv("""
        "value"
        1
        """))
