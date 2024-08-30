#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, Path, DataPath
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


# These diff tests use some locally collected trace.
class Instruments(TestSuite):

  def test_xml_stacks(self):
    return DiffTestBlueprint(
        trace=DataPath('instruments_trace.xml'),
        query='''
          WITH
            child AS (
              SELECT
                spc.id AS root,
                spc.id,
                spc.parent_id,
                rel_pc AS path
              FROM
                instruments_sample s
                JOIN stack_profile_callsite spc ON (s.callsite_id = spc.id)
                JOIN stack_profile_frame f ON (f.id = frame_id)
              UNION ALL
              SELECT
                child.root,
                parent.id,
                parent.parent_id,
                COALESCE(f.rel_pc || ',', '') || child.path AS path
              FROM
                child
                JOIN stack_profile_callsite parent ON (child.parent_id = parent.id)
                LEFT JOIN stack_profile_frame f ON (f.id = frame_id)
            )
          SELECT
            s.id,
            s.ts,
            s.utid,
            c.path
          FROM
            instruments_sample s
            JOIN child c ON s.callsite_id = c.root
          WHERE
            c.parent_id IS NULL
        ''',
        out=Csv('''
          "id","ts","utid","path"
          0,175685291,1,"23999,34891,37935,334037"
          1,176684208,1,"24307,28687,265407,160467,120123,391295,336787,8955,340991,392555,136711,5707,7603,10507,207839,207495,23655,17383,23211,208391,6225"
          2,177685166,1,"24915,16095,15891,32211,91151,26907,87887,60651,28343,29471,30159,11087,36269"
          3,178683916,1,"24915,16107,16047,16047,16047,16047,16047,16047,16047,16047,16047,16047,16047,16047,16047,16021"
          4,179687000,1,"24915,16107,16047,16047,16047,16047,16047,16047,16047,16047,16047,16047,16047,16047,16047,16047,16047,16005"
          5,180683708,1,"24915,16107,16047,16047,16047,16047,16047,16047,16047,16047,16047,16047,16047,16047,16047,16005"
        '''))

  def test_symbolized_frames(self):
    return DiffTestBlueprint(
        trace=DataPath('instruments_trace_with_symbols.zip'),
        query='''
          SELECT
            f.id,
            m.name,
            m.build_id,
            f.rel_pc,
            s.name,
            s.source_file,
            s.line_number
          FROM
            stack_profile_frame f
            JOIN stack_profile_mapping m ON f.mapping = m.id
            JOIN stack_profile_symbol s ON f.symbol_set_id = s.symbol_set_id
        ''',
        out=Csv('''
          "id","name","build_id","rel_pc","name","source_file","line_number"
          26,"/private/tmp/test","c3b3bdbd348730f18f9ddd08b7708d49",16095,"main","/tmp/test.cpp",25
          27,"/private/tmp/test","c3b3bdbd348730f18f9ddd08b7708d49",15891,"EmitSignpost()","/tmp/test.cpp",8
          38,"/private/tmp/test","c3b3bdbd348730f18f9ddd08b7708d49",16107,"main","/tmp/test.cpp",27
          39,"/private/tmp/test","c3b3bdbd348730f18f9ddd08b7708d49",16047,"fib(int)","/tmp/test.cpp",21
          40,"/private/tmp/test","c3b3bdbd348730f18f9ddd08b7708d49",16021,"fib(int)","/tmp/test.cpp",22
          41,"/private/tmp/test","c3b3bdbd348730f18f9ddd08b7708d49",16005,"fib(int)","/tmp/test.cpp",15
        '''))

  def test_symbolized_stacks(self):
    return DiffTestBlueprint(
        trace=DataPath('instruments_trace_with_symbols.zip'),
        query='''
          WITH
            frame AS (
              SELECT
                f.id AS frame_id,
                COALESCE(s.name || ':' || s.line_number, f.rel_pc) as name
              FROM
                stack_profile_frame f
                LEFT JOIN stack_profile_symbol s USING (symbol_set_id)
            ),
            child AS (
              SELECT
                spc.id AS root,
                spc.id,
                spc.parent_id,
                name AS path
              FROM
                instruments_sample s
                JOIN stack_profile_callsite spc ON (s.callsite_id = spc.id)
                LEFT JOIN frame f USING (frame_id)
              UNION ALL
              SELECT
                child.root,
                parent.id,
                parent.parent_id,
                COALESCE(f.name || ',', '') || child.path AS path
              FROM
                child
                JOIN stack_profile_callsite parent ON (child.parent_id = parent.id)
                LEFT JOIN frame f USING (frame_id)
            )
          SELECT
            s.id,
            s.ts,
            s.utid,
            c.path
          FROM
            instruments_sample s
            JOIN child c ON s.callsite_id = c.root
          WHERE
            c.parent_id IS NULL
        ''',
        out=Csv('''
          "id","ts","utid","path"
          0,175685291,1,"23999,34891,37935,334037"
          1,176684208,1,"24307,28687,265407,160467,120123,391295,336787,8955,340991,392555,136711,5707,7603,10507,207839,207495,23655,17383,23211,208391,6225"
          2,177685166,1,"24915,main:25,EmitSignpost():8,32211,91151,26907,87887,60651,28343,29471,30159,11087,36269"
          3,178683916,1,"24915,main:27,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):22"
          4,179687000,1,"24915,main:27,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):15"
          5,180683708,1,"24915,main:27,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):21,fib(int):15"
        '''))
