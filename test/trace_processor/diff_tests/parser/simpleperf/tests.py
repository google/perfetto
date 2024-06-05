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


# These diff tests are based on the same test data simpleperf uses for its
# testing
# (https://android.googlesource.com/platform/system/extras/+/refs/heads/main/simpleperf/testdata).
# Basically we load these perf files and make sure we can get the same data we
# would get via `simpleperf report`
class Simpleperf(TestSuite):
  # simpleperf report -i perf.data --print-event-count --csv --cpu 2,6,7
  def test_perf(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/perf.data'),
        query=Path('perf_test.sql'),
        out=Csv('''
        "event_count","command","pid","tid","shared_object","symbol"
        130707953,"t2",26130,26130,"/t2","t2[+51c]"
        126249237,"elf",26083,26083,"/elf","elf[+51c]"
        109687208,"t1",26124,26124,"/t1","t1[+523]"
        107027760,"t1",26124,26124,"/t1","t1[+51c]"
        101887409,"t2",26130,26130,"/t2","t2[+523]"
        92421568,"elf",26083,26083,"/elf","elf[+523]"
        61539363,"t1",26124,26124,"/t1","t1[+518]"
        60355129,"elf",26083,26083,"/elf","elf[+513]"
        54840659,"t1",26124,26124,"/t1","t1[+4ed]"
        52233968,"elf",26083,26083,"/elf","elf[+4ed]"
        50833094,"t1",26124,26124,"/t1","t1[+4f7]"
        50746374,"t2",26130,26130,"/t2","t2[+4ed]"
        49185691,"elf",26083,26083,"/elf","elf[+4f7]"
        47520901,"t2",26130,26130,"/t2","t2[+513]"
        45979652,"elf",26083,26083,"/elf","elf[+518]"
        44834371,"t2",26130,26130,"/t2","t2[+4f7]"
        42928068,"t2",26130,26130,"/t2","t2[+518]"
        39608138,"t1",26124,26124,"/t1","t1[+513]"
        1390415,"t1",26124,26124,"/t1","t1[+4fa]"
        1390305,"t2",26130,26130,"/t2","t2[+4fa]"
        1390173,"elf",26083,26083,"/elf","elf[+500]"
        1389030,"t2",26130,26130,"/t2","t2[+500]"
        693786,"t2",26130,26130,"/lib/modules/3.13.0-76-generic/kernel/drivers/ata/pata_acpi.ko","pata_acpi.ko[+ffffffffa05c4da4]"
        '''))

  def test_perf_tracks(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/perf.data'),
        query='''
        SELECT
          name,
          unit,
          description,
          cpu,
          is_timebase
        FROM perf_counter_track
        ORDER BY perf_session_id, name, cpu;
        ''',
        out=Csv('''
        "name","unit","description","cpu","is_timebase"
        "","","",2,1
        "","","",6,1
        "","","",7,1
        "","","",16,1
        '''))

  def test_perf_with_add_counter_tracks(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/perf_with_add_counter.data'),
        query='''
        SELECT
          name,
          unit,
          description,
          cpu,
          is_timebase
        FROM perf_counter_track
        ORDER BY perf_session_id, name, cpu;
        ''',
        out=Csv('''
        "name","unit","description","cpu","is_timebase"
        "cpu-cycles","","",40,1
        "instructions","","",40,0
        '''))

  # simpleperf report -i perf.data --print-event-count --csv
  # The thread name in this trace changes over time. simpleperf shows samples
  # with the old and new name. Perfetto does not support threads changing names,
  # it only keeps the last name, thus there is a slight mismatch in the outputs.
  def test_perf_with_add_counter(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/perf_with_add_counter.data'),
        query=Path('perf_with_add_counter_test.sql'),
        out=Csv('''
        "cpu_cycles","instructions","others","command","pid","tid","shared_object","symbol"
        1011567,1188389,0,"sleep",689664,689664,"[kernel.kallsyms]","[kernel.kallsyms][+ffffffffa8cc9d30]"
        219490,233619,0,"sleep",689664,689664,"[kernel.kallsyms]","[kernel.kallsyms][+ffffffffa8e498c6]"
        191017,157031,0,"sleep",689664,689664,"[kernel.kallsyms]","[kernel.kallsyms][+ffffffffa94d0901]"
        175099,140443,0,"sleep",689664,689664,"/lib/x86_64-linux-gnu/libc-2.32.so","_dl_addr"
        152310,130151,0,"sleep",689664,689664,"[kernel.kallsyms]","[kernel.kallsyms][+ffffffffa8e30c70]"
        122439,87058,0,"sleep",689664,689664,"[kernel.kallsyms]","[kernel.kallsyms][+ffffffffa960015d]"
        89368,68332,0,"sleep",689664,689664,"[kernel.kallsyms]","[kernel.kallsyms][+ffffffffa8e03757]"
        40272,30457,0,"sleep",689664,689664,"/lib/x86_64-linux-gnu/ld-2.32.so","ld-2.32.so[+1767b]"
        14742,7858,0,"sleep",689664,689664,"[kernel.kallsyms]","[kernel.kallsyms][+ffffffffa8ce7a78]"
        7551,1953,0,"sleep",689664,689664,"[kernel.kallsyms]","[kernel.kallsyms][+ffffffffa8cc90c5]"
        7080,2940,0,"sleep",689664,689664,"[kernel.kallsyms]","[kernel.kallsyms][+ffffffffa8cc8119]"
        3520,295,0,"sleep",689664,689664,"[kernel.kallsyms]","[kernel.kallsyms][+ffffffffa8c6b3e6]"
        '''))

  def test_build_id_feature(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/perf.data'),
        query='''
        SELECT build_id, name
        FROM stack_profile_mapping
        WHERE build_id <> ""
        ORDER BY name
        ''',
        out=Csv('''
        "build_id","name"
        "0b12a384a9f4a3f3659b7171ca615dbec3a81f71","/elf"
        "0b12a384a9f4a3f3659b7171ca615dbec3a81f71","/elf"
        "47111a47babdcd27ca2f9ff450dc1897ded761ed","/lib/modules/3.13.0-76-generic/kernel/drivers/ata/pata_acpi.ko"
        "0b12a384a9f4a3f3659b7171ca615dbec3a81f71","/t1"
        "0b12a384a9f4a3f3659b7171ca615dbec3a81f71","/t2"
        '''))

  def test_clocks_align(self):
    return DiffTestBlueprint(
        trace=DataPath('zip/perf_track_sym.zip'),
        query=Path('clocks_align_test.sql'),
        out=Csv('''
        "misaligned_count"
        0
        '''))

  def test_cmdline(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/perf.data'),
        query='''
        SELECT cmdline
        FROM perf_session
        ''',
        out=Csv('''
        "cmdline"
        "/ssd/android/aosp_master/out/host/linux-x86/bin/simpleperf record -p 26083,26090,26124,26130 sleep 0.0001"
        '''))
