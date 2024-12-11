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


class Etm(TestSuite):

  def test_sessions(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/cs_etm_u.perf'),
        query='''
          SELECT start_ts, cpu, size
          FROM
            __intrinsic_etm_v4_configuration AS C,
             __intrinsic_etm_v4_session AS s
             ON c.id = s.configuration_id,
             __intrinsic_etm_v4_trace AS t
             ON s.id = t.session_id
          WHERE start_ts < 21077000721310
          ORDER BY start_ts ASC
        ''',
        out=Csv('''
         "start_ts","cpu","size"
          21076849718299,8,238
          21076860480953,8,238
          21076871238359,8,238
          21076882058915,8,238
          21076892817338,8,238
          21076903628740,8,238
          21076914454749,8,238
          21076924779741,8,238
          21076935681637,8,238
          21076946070594,8,360
          21076956953243,8,238
          21076967922726,8,238
          21076978919593,8,238
          21076989801225,8,356
        '''))

  def test_decode_all(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/cs_etm_u.perf'),
        query='''
          SELECT count(*)
          FROM
            __intrinsic_etm_v4_trace t,
            __intrinsic_etm_decode_trace d
            ON t.id = d.trace_id
        ''',
        out=Csv('''
          "count(*)"
          5871
        '''))

  def test_decode_trace(self):
    return DiffTestBlueprint(
        register_files_dir=DataPath('simpleperf/bin'),
        trace=DataPath('simpleperf/cs_etm_u.perf'),
        query='''
          SELECT *
          FROM
            __intrinsic_etm_decode_trace
          WHERE trace_id = 0
        ''',
        out=Path('decode_trace.out'))

  def test_iterate_instructions(self):
    return DiffTestBlueprint(
        register_files_dir=DataPath('simpleperf/bin'),
        trace=DataPath('simpleperf/cs_etm_u.perf'),
        query='''
          SELECT d.element_index, i.*
          FROM
            __intrinsic_etm_decode_trace d,
            __intrinsic_etm_iterate_instruction_range i
            USING(instruction_range)
          WHERE trace_id = 0 AND mapping_id = 1
        ''',
        out=Path('iterate_instructions.out'))
