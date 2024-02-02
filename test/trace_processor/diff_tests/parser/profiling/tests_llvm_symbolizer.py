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
        LIMIT 10;
        """,
        out=Csv('''
          "id","type","ts","depth","name","map_name","count","cumulative_count","size","cumulative_size","alloc_count","cumulative_alloc_count","alloc_size","cumulative_alloc_size","parent_id","source_file","line_number"
          0,"experimental_flamegraph",7689491063351,0,"__start_thread","/apex/com.android.runtime/lib64/bionic/libc.so",0,560,0,560,0,0,0,0,"[NULL]","[NULL]","[NULL]"
          1,"experimental_flamegraph",7689491063351,1,"_ZL15__pthread_startPv","/apex/com.android.runtime/lib64/bionic/libc.so",0,560,0,560,0,0,0,0,0,"[NULL]","[NULL]"
          2,"experimental_flamegraph",7689491063351,2,"_ZN3art6Thread14CreateCallbackEPv","/apex/com.android.art/lib64/libart.so",0,301,0,301,0,0,0,0,1,"[NULL]","[NULL]"
          3,"experimental_flamegraph",7689491063351,3,"_ZN3art35InvokeVirtualOrInterfaceWithJValuesIPNS_9ArtMethodEEENS_6JValueERKNS_33ScopedObjectAccessAlreadyRunnableEP8_jobjectT_PK6jvalue","/apex/com.android.art/lib64/libart.so",0,301,0,301,0,0,0,0,2,"[NULL]","[NULL]"
          4,"experimental_flamegraph",7689491063351,4,"_ZN3art9ArtMethod6InvokeEPNS_6ThreadEPjjPNS_6JValueEPKc","/apex/com.android.art/lib64/libart.so",0,301,0,301,0,0,0,0,3,"[NULL]","[NULL]"
          5,"experimental_flamegraph",7689491063351,5,"art_quick_invoke_stub","/apex/com.android.art/lib64/libart.so",0,301,0,301,0,0,0,0,4,"[NULL]","[NULL]"
          6,"experimental_flamegraph",7689491063351,6,"android.os.HandlerThread.run","/system/framework/arm64/boot-framework.oat",0,43,0,43,0,0,0,0,5,"[NULL]","[NULL]"
          7,"experimental_flamegraph",7689491063351,7,"android.os.Looper.loop","/system/framework/arm64/boot-framework.oat",0,43,0,43,0,0,0,0,6,"[NULL]","[NULL]"
          8,"experimental_flamegraph",7683950792832,8,"android.os.Looper.loopOnce","/system/framework/arm64/boot-framework.oat",1,43,1,43,0,0,0,0,7,"[NULL]","[NULL]"
          9,"experimental_flamegraph",7689491063351,9,"android.os.Handler.dispatchMessage","/system/framework/arm64/boot-framework.oat",0,35,0,35,0,0,0,0,8,"[NULL]","[NULL]"
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
