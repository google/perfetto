#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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


class Etm(TestSuite):

  def test_sessions(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/cs_etm_u.perf'),
        module_dependencies=['etm'],
        query='''
          SELECT start_ts, cpu, size
          FROM
            __intrinsic_etm_v4_configuration AS C,
             __intrinsic_etm_v4_session AS s
             ON c.id = s.configuration_id,
             __intrinsic_etm_v4_chunk AS t
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
        module_dependencies=['etm'],
        query='''
          SELECT count(*)
          FROM
            __intrinsic_etm_v4_chunk t,
            __intrinsic_etm_decode_chunk d
            ON t.id = d.chunk_id
        ''',
        out=Csv('''
          "count(*)"
          5871
        '''))

  def test_decode_chunk(self):
    return DiffTestBlueprint(
        register_files_dir=DataPath('simpleperf/bin'),
        trace=DataPath('simpleperf/cs_etm_u.perf'),
        module_dependencies=['etm'],
        query='''
          SELECT
            chunk_index, element_index, element_type, timestamp, cycle_count,
            last_seen_timestamp, cumulative_cycles, exception_level,
            context_id, isa, start_address, end_address, mapping_id
          FROM
            __intrinsic_etm_decode_chunk
          WHERE chunk_id = 0
        ''',
        out=Path('decode_chunk.out'))

  def test_iterate_instructions(self):
    return DiffTestBlueprint(
        register_files_dir=DataPath('simpleperf/bin'),
        trace=DataPath('simpleperf/cs_etm_u.perf'),
        module_dependencies=['etm'],
        query='''
          SELECT d.element_index, i.*
          FROM
            __intrinsic_etm_decode_chunk d,
            __intrinsic_etm_iterate_instruction_range i
            USING(instruction_range)
          WHERE chunk_id = 0 AND mapping_id = 1
        ''',
        out=Path('iterate_instructions.out'))

  def test_etm_metadata(self):
    return DiffTestBlueprint(
        register_files_dir=DataPath('simpleperf/bin'),
        trace=DataPath('simpleperf/cs_etm_u.perf'),
        module_dependencies=['etm'],
        query='''
          INCLUDE PERFETTO MODULE linux.perf.etm;
          SELECT
            replace(file_name, rtrim(file_name, replace(file_name, '/', '')), '') AS short_file_name,
            rel_pc,
            mapping_id,
            address
          FROM _linux_perf_etm_metadata(0)
          WHERE short_file_name="etm"
        ''',
        out=Csv('''
          "short_file_name","rel_pc","mapping_id","address"
          "etm",18504,1,434500225096
          "etm",18508,1,434500225100
          "etm",18512,1,434500225104
          "etm",18492,1,434500225084
          "etm",18536,1,434500225128
          "etm",18540,1,434500225132
          "etm",18544,1,434500225136
          "etm",18548,1,434500225140
          "etm",18552,1,434500225144
          "etm",18556,1,434500225148
          "etm",18560,1,434500225152
          "etm",18564,1,434500225156
          "etm",18568,1,434500225160
          "etm",18976,1,434500225568
          "etm",18980,1,434500225572
          "etm",18984,1,434500225576
          "etm",18988,1,434500225580
        '''))

  def test_last_seen_ts_and_cumilative_cc(self):
    return DiffTestBlueprint(
        register_files_dir=DataPath('simpleperf/bin'),
        trace=DataPath('simpleperf/cs_etm_cc_ts.perf'),
        module_dependencies=['etm'],
        query='''
        CREATE PERFETTO TABLE decoded_chunk_one AS
        SELECT
          chunk_index, element_index, element_type, timestamp, cycle_count,
          last_seen_timestamp, cumulative_cycles, exception_level,
          context_id, isa, start_address, end_address, mapping_id
        FROM
          __intrinsic_etm_decode_chunk(1);

        SELECT * FROM decoded_chunk_one
        LIMIT 100;
        ''',
        out=Path('ts_cc.out'))
