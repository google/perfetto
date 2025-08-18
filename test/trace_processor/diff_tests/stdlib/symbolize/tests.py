#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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


class Symbolize(TestSuite):

  def test_callstack_frame_symbolize_etm_tables(self):
    return DiffTestBlueprint(
        register_files_dir=DataPath('simpleperf/bin'),
        trace=DataPath('simpleperf/cs_etm_u.perf'),
        module_dependencies=['llvm_symbolizer', 'etm'],
        query="""
        INCLUDE PERFETTO MODULE callstacks.symbolize;
        INCLUDE PERFETTO MODULE linux.perf.etm;

        SELECT
          function_name,
          replace(file_name, rtrim(file_name, replace(file_name, '/', '')), '') AS short_file_nam,
          line_number,
          mapping_id,
          address
        FROM _callstack_frame_symbolize!(
            _linux_perf_etm_metadata(0)
            WHERE mapping_id = 1
        );
        """,
        out=Csv("""
        "function_name","short_file_nam","line_number","mapping_id","address"
        "main","etm.cc",62,1,434500225096
        "main","etm.cc",0,1,434500225100
        "main","etm.cc",62,1,434500225104
        "main","etm.cc",60,1,434500225084
        "A()","etm.cc",44,1,434500225128
        "A()","etm.cc",44,1,434500225132
        "A()","etm.cc",44,1,434500225136
        "A()","etm.cc",46,1,434500225140
        "A()","etm.cc",46,1,434500225144
        "A()","etm.cc",47,1,434500225148
        "A()","etm.cc",47,1,434500225152
        "A()","etm.cc",47,1,434500225156
        "A()","etm.cc",47,1,434500225160
        "<invalid>","<invalid>",0,1,434500225568
        "<invalid>","<invalid>",0,1,434500225572
        "<invalid>","<invalid>",0,1,434500225576
        "<invalid>","<invalid>",0,1,434500225580
        """))

  def test_callstack_frame_symbolize_etm_subquery(self):
    return DiffTestBlueprint(
        register_files_dir=DataPath('simpleperf/bin'),
        trace=DataPath('simpleperf/cs_etm_u.perf'),
        module_dependencies=['llvm_symbolizer', 'etm'],
        query="""
        INCLUDE PERFETTO MODULE callstacks.symbolize;

        SELECT
          function_name,
          replace(file_name, rtrim(file_name, replace(file_name, '/', '')), '') AS short_file_nam,
          line_number,
          mapping_id,
          address
        FROM _callstack_frame_symbolize!((
            SELECT
            __intrinsic_file.name AS file_name,
            __intrinsic_etm_iterate_instruction_range.address - stack_profile_mapping.start + stack_profile_mapping.exact_offset + __intrinsic_elf_file.load_bias AS rel_pc,
            __intrinsic_etm_decode_chunk.mapping_id AS mapping_id,
            __intrinsic_etm_iterate_instruction_range.address AS address
            FROM __intrinsic_etm_decode_chunk(0)
            JOIN __intrinsic_etm_iterate_instruction_range
              ON __intrinsic_etm_decode_chunk.instruction_range = __intrinsic_etm_iterate_instruction_range.instruction_range
            JOIN stack_profile_mapping
              ON __intrinsic_etm_decode_chunk.mapping_id = stack_profile_mapping.id
            JOIN __intrinsic_elf_file
              ON stack_profile_mapping.build_id = __intrinsic_elf_file.build_id
            JOIN __intrinsic_file
              ON __intrinsic_elf_file.file_id = __intrinsic_file.id
            WHERE mapping_id = 1
        ));
        """,
        out=Csv("""
        "function_name","short_file_nam","line_number","mapping_id","address"
        "main","etm.cc",62,1,434500225096
        "main","etm.cc",0,1,434500225100
        "main","etm.cc",62,1,434500225104
        "main","etm.cc",60,1,434500225084
        "A()","etm.cc",44,1,434500225128
        "A()","etm.cc",44,1,434500225132
        "A()","etm.cc",44,1,434500225136
        "A()","etm.cc",46,1,434500225140
        "A()","etm.cc",46,1,434500225144
        "A()","etm.cc",47,1,434500225148
        "A()","etm.cc",47,1,434500225152
        "A()","etm.cc",47,1,434500225156
        "A()","etm.cc",47,1,434500225160
        "<invalid>","<invalid>",0,1,434500225568
        "<invalid>","<invalid>",0,1,434500225572
        "<invalid>","<invalid>",0,1,434500225576
        "<invalid>","<invalid>",0,1,434500225580
        """))

  def test_callstack_frame_symbolize(self):
    return DiffTestBlueprint(
        register_files_dir=DataPath('simpleperf/bin'),
        trace=DataPath('simpleperf/cs_etm_u.perf'),
        module_dependencies=['llvm_symbolizer'],
        query="""
        INCLUDE PERFETTO MODULE callstacks.symbolize;

        SELECT
          function_name,
          replace(file_name, rtrim(file_name, replace(file_name, '/', '')), '') AS short_file_name,
          line_number,
          mapping_id,
          address
        FROM _callstack_frame_symbolize!((
            SELECT
              dynamic_file.name AS file_name,
              static_data.rel_pc,
              static_data.mapping_id,
              static_data.address
            FROM
              (
                SELECT name
                FROM __intrinsic_file
                WHERE name GLOB '*/bin/etm'
                LIMIT 1
              ) AS dynamic_file
            CROSS JOIN
              -- This section has been modified to be compatible
              (
                SELECT
                  column1 AS rel_pc,
                  column2 AS mapping_id,
                  column3 AS address
                FROM (
                  VALUES
                    (18504,1,434500225096),
                    (18508,1,434500225100),
                    (18512,1,434500225104),
                    (18492,1,434500225084),
                    (18536,1,434500225128),
                    (18540,1,434500225132),
                    (18544,1,434500225136),
                    (18548,1,434500225140),
                    (18552,1,434500225144),
                    (18556,1,434500225148),
                    (18560,1,434500225152),
                    (18564,1,434500225156),
                    (18568,1,434500225160),
                    (18976,1,434500225568),
                    (18980,1,434500225572),
                    (18984,1,434500225576),
                    (18988,1,434500225580)
                )
              ) AS static_data
        ));
        """,
        out=Csv("""
        "function_name","short_file_name","line_number","mapping_id","address"
        "main","etm.cc",62,1,434500225096
        "main","etm.cc",0,1,434500225100
        "main","etm.cc",62,1,434500225104
        "main","etm.cc",60,1,434500225084
        "A()","etm.cc",44,1,434500225128
        "A()","etm.cc",44,1,434500225132
        "A()","etm.cc",44,1,434500225136
        "A()","etm.cc",46,1,434500225140
        "A()","etm.cc",46,1,434500225144
        "A()","etm.cc",47,1,434500225148
        "A()","etm.cc",47,1,434500225152
        "A()","etm.cc",47,1,434500225156
        "A()","etm.cc",47,1,434500225160
        "<invalid>","<invalid>",0,1,434500225568
        "<invalid>","<invalid>",0,1,434500225572
        "<invalid>","<invalid>",0,1,434500225576
        "<invalid>","<invalid>",0,1,434500225580
        """))
