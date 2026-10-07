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
        SELECT
          ef.ts,
          ef.depth,
          ef.name,
          ef.map_name,
          ef.count,
          ef.cumulative_count,
          ef.size,
          ef.cumulative_size,
          alloc_count,
          cumulative_alloc_count,
          alloc_size,
          cumulative_alloc_size,
          ef.source_file,
          ef.line_number
        FROM process
        JOIN experimental_flamegraph(
          'perf',
          NULL,
          '<=7689491063351',
          process.upid,
          NULL,
          NULL
        ) ef
        WHERE pid = 1728
        ORDER BY ef.depth, ef.name, ef.map_name, ef.cumulative_size,
                 ef.cumulative_count
        LIMIT 10;
        """,
        out=Csv('''
          "ts","depth","name","map_name","count","cumulative_count","size","cumulative_size","alloc_count","cumulative_alloc_count","alloc_size","cumulative_alloc_size","source_file","line_number"
          7689491063351,0,"ERROR INVALID_MAP","/ERROR",0,1,0,1,0,0,0,0,"[NULL]","[NULL]"
          7689491063351,0,"__libc_init","/apex/com.android.runtime/lib64/bionic/libc.so",0,10,0,10,0,0,0,0,"[NULL]","[NULL]"
          7689491063351,0,"__start_thread","/apex/com.android.runtime/lib64/bionic/libc.so",0,560,0,560,0,0,0,0,"[NULL]","[NULL]"
          7689491063351,0,"clone","/apex/com.android.runtime/lib64/bionic/libc.so",0,1,0,1,0,0,0,0,"[NULL]","[NULL]"
          7689491063351,1,"","",0,1,0,1,0,0,0,0,"[NULL]","[NULL]"
          7689491063351,1,"_ZL15__pthread_startPv","/apex/com.android.runtime/lib64/bionic/libc.so",0,560,0,560,0,0,0,0,"[NULL]","[NULL]"
          7689491063351,1,"__bionic_clone","/apex/com.android.runtime/lib64/bionic/libc.so",0,1,0,1,0,0,0,0,"[NULL]","[NULL]"
          7689491063351,1,"main","/system/bin/app_process64",0,10,0,10,0,0,0,0,"[NULL]","[NULL]"
          7689491063351,1,"main","/system/bin/surfaceflinger",0,0,0,0,0,0,0,0,"[NULL]","[NULL]"
          7689491063351,2,"_ZN13thread_data_t10trampolineEPKS_","/system/lib64/libutils.so",0,246,0,246,0,0,0,0,"[NULL]","[NULL]"
        '''))

  def test_callstack_sampling_flamegraph_multi_process(self):
    return DiffTestBlueprint(
        trace=DataPath('callstack_sampling.pftrace'),
        query="""
        SELECT count(*) AS count, 'BothProcesses' AS description
        FROM experimental_flamegraph(
          'perf',
          NULL,
          '<=7689491063351',
          NULL,
          (
            SELECT group_concat(DISTINCT upid)
            FROM perf_sample
            JOIN thread t USING (utid)
            JOIN process p USING (upid)
          ),
          NULL
        )
        WHERE size > 0
        UNION ALL
        SELECT count(*) AS count, 'FirstProcess' AS description
        FROM process
        JOIN experimental_flamegraph(
          'perf',
          NULL,
          '<=7689491063351',
          process.upid,
          NULL,
          NULL
        )
        WHERE pid = 1728 AND size > 0
        UNION ALL
        SELECT count(*) AS count, 'SecondProcess' AS description
        FROM process
        JOIN experimental_flamegraph(
          'perf',
          NULL,
          '<=7689491063351',
          process.upid,
          NULL,
          NULL
        )
        WHERE pid = 703 AND size > 0;
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
