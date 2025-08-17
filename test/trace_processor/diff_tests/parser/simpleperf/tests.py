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
          "","[NULL]","[NULL]",2,1
          "","[NULL]","[NULL]",6,1
          "","[NULL]","[NULL]",7,1
          "","[NULL]","[NULL]",16,1
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
          "cpu-cycles","[NULL]","[NULL]",40,1
          "instructions","[NULL]","[NULL]",40,0
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

  # Make sure we can parse perf.data files with synthetic events (perf will
  # write those with an id = 0). This trace file has some synthetic COMM events.
  def test_perf_with_synthetic_events(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/perf_with_synthetic_events.data'),
        query='''
        SELECT tid, name
        FROM thread
        ORDER BY tid
        ''',
        out=Csv('''
        "tid","name"
        0,"[NULL]"
        289003,"trace_processor"
        '''))

  def test_perf_with_no_cpu_in_sample(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/perf_with_synthetic_events.data'),
        query='''
        SELECT cpu, COUNT(*) AS count
        FROM perf_sample
        WHERE callsite_id IS NOT NULL
        GROUP BY cpu ORDER BY cpu
        ''',
        out=Csv('''
        "cpu","count"
        "[NULL]",9126
        '''))

  def test_linux_perf_unwinding(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/linux_perf_with_symbols.zip'),
        query=Path('stacks_test.sql'),
        out=Csv('''
        "name"
        "main,A"
        "main,A,B"
        "main,A,B,C"
        "main,A,B,C,D"
        "main,A,B,C,D,E"
        "main,A,B,C,E"
        "main,A,B,D"
        "main,A,B,D,E"
        "main,A,B,E"
        "main,A,C"
        "main,A,C,D"
        "main,A,C,D,E"
        "main,A,C,E"
        "main,A,D"
        "main,A,D,E"
        "main,A,E"
        "main,B"
        "main,B,C"
        "main,B,C,D"
        "main,B,C,D,E"
        "main,B,C,E"
        "main,B,D"
        "main,B,D,E"
        "main,B,E"
        "main,C"
        "main,C,D"
        "main,C,D,E"
        "main,C,E"
        "main,D"
        "main,D,E"
        "main,E"
        '''))

  def test_jit_symbolization(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/perf_with_jit_and_apk.data'),
        query='''
        SELECT
          frame.name as frame_name
        FROM stack_profile_frame frame
        JOIN stack_profile_mapping mapping ON frame.mapping = mapping.id
        WHERE
          mapping.name GLOB '*jit*'
          AND frame.name IS NOT NULL
        ORDER BY frame.name;
        ''',
        out=Csv('''
        "frame_name"
        "abh.a"
        "android.support.v7.widget.GridLayoutManager.layoutChunk"
        "android.support.v7.widget.SwitchCompat.draw"
        "android.support.v7.widget.SwitchCompat.draw"
        "android.support.v7.widget.SwitchCompat.draw"
        "androidx.constraintlayout.widget.ConstraintLayout.L"
        "androidx.constraintlayout.widget.ConstraintLayout.L"
        "androidx.constraintlayout.widget.ConstraintLayout.onMeasure"
        "ccs.<init>"
        "ccs.<init>"
        "ccs.<init>"
        "ccs.<init>"
        "ccs.<init>"
        "ccs.<init>"
        "ccs.<init>"
        "ccs.<init>"
        "ccs.<init>"
        "ccs.<init>"
        "ccs.<init>"
        "cht.B"
        "cht.L"
        "cht.L"
        "frj.b"
        "frj.draw"
        "frj.draw"
        "frj.draw"
        "frj.draw"
        "frj.i"
        "frn.B"
        "frr.b"
        "frr.b"
        "frr.b"
        "frr.b"
        "frr.c"
        "frr.c"
        "frz.c"
        "hf.a"
        "hvx.S"
        "hvx.T"
        "mt.k"
        "pi.a"
        "pi.a"
        "rp.g"
        "wg.k"
        "xi.Y"
        "xi.Y"
        "xi.Y"
        "xi.ae"
        "xx.a"
        "xx.b"
        "xx.b"
        "xx.b"
        '''))

  def test_spe_operation(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/spe.trace.zip'),
        query='''
        INCLUDE PERFETTO MODULE linux.perf.spe;
        SELECT
          operation,
          count(*) AS cnt
        FROM linux_perf_spe_record
        GROUP BY operation
        ORDER BY operation
        ''',
        out=Csv('''
        "operation","cnt"
        "BRANCH",68038
        "LOAD",54
        "STORE",47
        '''))

  def test_spe_pc(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/spe.trace.zip'),
        query='''
        INCLUDE PERFETTO MODULE linux.perf.spe;
        SELECT
          printf('0x%08x', rel_pc + m.start - exact_offset) AS pc,
          exception_level,
          COUNT(*) AS cnt
        FROM linux_perf_spe_record r, stack_profile_frame f
        ON r.instruction_frame_id = f.id,
        stack_profile_mapping m
        ON f.mapping = m.id
        GROUP BY pc, exception_level
        HAVING cnt > 1
        ORDER BY pc, exception_level
        ''',
        out=Csv('''
        "pc","exception_level","cnt"
        "0x5cfc344464","EL0",2157
        "0x5cfc344528","EL0",2166
        "0x5cfc3445c4","EL0",2154
        "0x5cfc3446c8","EL0",2108
        "0x5cfc3447a8","EL0",2209
        "0x5cfc344854","EL0",2178
        "0x5cfc34492c","EL0",2246
        "0x5cfc344c14","EL0",4461
        "0x5cfc344cd0","EL0",4416
        "0x5cfc344d7c","EL0",4399
        "0x5cfc344df4","EL0",2
        "0x5cfc344e90","EL0",4427
        "0x5cfc3450e8","EL0",8756
        "0x5cfc345194","EL0",8858
        "0x5cfc345240","EL0",8776
        "0x5cfc345354","EL0",8659
        "0xffffd409990628","EL1",14
        "0xffffd40999062c","EL1",15
        "0xffffd40fb0f124","EL1",2
        '''))

  def test_perf_summary_tree(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/perf.data'),
        query='''
          INCLUDE PERFETTO MODULE linux.perf.samples;

          SELECT *
          FROM linux_perf_samples_summary_tree
          LIMIT 10
        ''',
        out=Csv('''
          "id","parent_id","name","mapping_name","source_file","line_number","self_count","cumulative_count"
          0,"[NULL]","","/elf","[NULL]","[NULL]",84,84
          1,"[NULL]","","/elf","[NULL]","[NULL]",69,69
          2,"[NULL]","","/elf","[NULL]","[NULL]",177,177
          3,"[NULL]","","/elf","[NULL]","[NULL]",89,89
          4,"[NULL]","","/t1","[NULL]","[NULL]",70,70
          5,"[NULL]","","/elf","[NULL]","[NULL]",218,218
          6,"[NULL]","","/elf","[NULL]","[NULL]",65,65
          7,"[NULL]","","/elf","[NULL]","[NULL]",70,70
          8,"[NULL]","","/t1","[NULL]","[NULL]",87,87
          9,"[NULL]","","/elf","[NULL]","[NULL]",64,64
        '''))

  def test_global_counters(self):
    return DiffTestBlueprint(
        # Interesting side-note is that the "perf record" implementation still
        # decided to open a separate counter on each CPU (and then mapped all of
        # them into a single buffer with ioctls) [1]. It may have had something
        # to do with inherit complexity.
        trace=DataPath('perf_counter.data'),
        query="""
        SELECT
          t.name AS track_name,
          c.ts,
          c.value
        FROM counter c
        JOIN perf_counter_track t
          ON c.track_id = t.id
        ORDER BY t.name, c.ts
        LIMIT 10;
        """,
        out=Csv("""
        "track_name","ts","value"
        "task-clock:ppp",1211974640655330,250000.000000
        "task-clock:ppp",1211974640904003,500000.000000
        "task-clock:ppp",1211974641154649,750000.000000
        "task-clock:ppp",1211974641405037,1000000.000000
        "task-clock:ppp",1211974641654472,1250000.000000
        "task-clock:ppp",1211974641903904,1500000.000000
        "task-clock:ppp",1211974642154097,1750000.000000
        "task-clock:ppp",1211974642404623,2000000.000000
        "task-clock:ppp",1211974642654928,2250000.000000
        "task-clock:ppp",1211974642904128,2500000.000000
        """))

  def test_apk_symbolization(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/perf_with_jit_and_apk.data'),
        query='''
        select spf.rel_pc, spf.name, spm.name, start, end
        from stack_profile_frame spf
        join stack_profile_mapping spm on spf.mapping = spm.id
        where spm.name glob '*apk*'
        limit 20
        ''',
        out=Csv('''
        "rel_pc","name","name","start","end"
        1223152,"ew.onStart","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        1152622,"bx.onStart","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        2327862,"boa.P","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        1170302,"co.ak","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        1152654,"bx.onStart","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        1168868,"co.C","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        1168962,"co.D","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        1175796,"co.K","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        1182724,"ct.d","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        2536682,"bxq.m","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        1935740,"ahh.h","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        2412604,"com.android.deskclock.DeskClock.onResume","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        1152530,"bx.onResume","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        1223096,"ew.onPostResume","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        1152424,"bx.onPostResume","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        1152460,"bx.onPostResume","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        1168824,"co.B","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        1182520,"ct.d","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        2409280,"bri.ac","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        2409764,"bri.x","/data/app/~~ZOaGUP2pmFbfL5N1tXwzQg==/com.google.android.deskclock-0_l2Nfy2tWQAnavJSkNABA==/base.apk",525645717504,525651664896
        '''))
