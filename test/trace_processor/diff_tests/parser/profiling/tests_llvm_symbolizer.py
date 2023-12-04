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
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ProfilingLlvmSymbolizer(TestSuite):
  # this uses llvm-symbolizer to test the offline symbolization built into
  def test_stack_profile_symbols(self):
    return DiffTestBlueprint(
        trace=DataPath('heapprofd_standalone_client_example-trace'),
        query="""
        SELECT name, source_file, line_number FROM stack_profile_symbol;
        """,
        out=Path('stack_profile_symbols.out'))

  def test_callstack_sampling_flamegraph(self):
    return DiffTestBlueprint(
        trace=DataPath('callstack_sampling.pftrace'),
        query="""
        SELECT ef.*
        FROM experimental_flamegraph ef
        JOIN process USING (upid)
        WHERE pid = 1728
          AND profile_type = 'perf'
          AND ts <= 7689491063351
        LIMIT 10;
        """,
        out=Path('callstack_sampling_flamegraph.out'))

  def test_callstack_sampling_flamegraph_multi_process(self):
    return DiffTestBlueprint(
        trace=DataPath('callstack_sampling.pftrace'),
        query="""
        SELECT count(*) AS count, 'BothProcesses' AS description
        FROM experimental_flamegraph
        WHERE
          upid_group = (
            SELECT group_concat(DISTINCT upid)
            FROM perf_sample JOIN thread t USING (utid) JOIN process p USING (upid)
          )
          AND profile_type = 'perf'
          AND ts <= 7689491063351
          AND size > 0
        UNION ALL
        SELECT count(*) AS count, 'FirstProcess' AS description
        FROM experimental_flamegraph
        JOIN process USING (upid)
        WHERE pid = 1728
          AND profile_type = 'perf'
          AND ts <= 7689491063351
          AND size > 0
        UNION ALL
        SELECT count(*) AS count, 'SecondProcess' AS description
        FROM experimental_flamegraph
        JOIN process USING (upid)
        WHERE pid = 703
          AND profile_type = 'perf'
          AND ts <= 7689491063351
          AND size > 0;
        """,
        out=Csv("""
        "count","description"
        658,"BothProcesses"
        483,"FirstProcess"
        175,"SecondProcess"
        """))

  def test_no_build_id(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_data_local_tmp.textproto'),
        query="""
        SELECT value FROM stats WHERE name = 'symbolization_tmp_build_id_not_found';
        """,
        out=Csv("""
        "value"
        1
        """))
