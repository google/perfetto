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

from python.generators.diff_tests.testing import Path, DataPath, Metric, Systrace
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from python.generators.diff_tests.testing import PrintProfileProto


class LinuxTests(TestSuite):

  def test_kernel_threads(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE linux.threads;

        SELECT pid, tid, process_name, thread_name
        FROM linux_kernel_threads
        ORDER by tid LIMIT 10;
        """,
        out=Csv("""
            "pid","tid","process_name","thread_name"
            2,2,"kthreadd","kthreadd"
            5,5,"kworker/0:0H","kworker/0:0H"
            6,6,"kworker/u16:0","kworker/u16:0"
            8,8,"kworker/u16:1","kworker/u16:1"
            11,11,"ksoftirqd/0","ksoftirqd/0"
            12,12,"rcu_preempt","rcu_preempt"
            13,13,"rcuog/0","rcuog/0"
            14,14,"rcuop/0","rcuop/0"
            15,15,"rcub/0","rcub/0"
            17,17,"rcu_exp_gp_kthr","rcu_exp_gp_kthr"
        """))

  # Tests that DSU devfreq counters are working properly
  def test_dsu_devfreq(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_tk4_pcmark.pb'),
        query=("""
            INCLUDE PERFETTO MODULE linux.devfreq;
            SELECT id, ts, dur, dsu_freq FROM linux_devfreq_dsu_counter
            LIMIT 20
            """),
        out=Csv("""
            "id","ts","dur","dsu_freq"
            61,4106584783742,11482788,610000
            166,4106596266530,8108602,1197000
            212,4106604375132,21453410,610000
            487,4106625828542,39427368,820000
            1130,4106665255910,3264242,610000
            1173,4106668520152,16966105,820000
            1391,4106685486257,10596883,970000
            1584,4106696083140,10051636,610000
            1868,4106706134776,14058960,820000
            2136,4106720193736,116719238,610000
            4388,4106836912974,8285848,1197000
            4583,4106845198822,16518433,820000
            5006,4106861717255,9357503,1328000
            5238,4106871074758,27228760,1197000
            5963,4106898303518,16581706,820000
            6498,4106914885224,9954142,1197000
            6763,4106924839366,9024780,970000
            7061,4106933864146,26264160,820000
            7637,4106960128306,11008505,970000
            7880,4106971136811,9282511,1197000
            """))

  def test_active_block_io_operations_by_device(self):
    return DiffTestBlueprint(
        trace=DataPath('linux_block_io_trace.pb'),
        query="""
        INCLUDE PERFETTO MODULE linux.block_io;

        SELECT
            ts,
            ops_in_queue_or_device,
            dev,
            linux_device_major_id(dev) as major,
            linux_device_minor_id(dev) as minor
        FROM linux_active_block_io_operations_by_device
        ORDER by ts
        LIMIT 20;
        """,
        out=Csv("""
        "ts","ops_in_queue_or_device","dev","major","minor"
        241211905210838,1,45824,179,0
        241211909452069,0,45824,179,0
        241211909585838,1,45824,179,0
        241211909845222,0,45824,179,0
        241211910299145,1,1795,7,3
        241211910636838,0,1795,7,3
        241211912818299,1,45824,179,0
        241211913170838,0,45824,179,0
        241211916130530,1,45824,179,0
        241211916325222,0,45824,179,0
        241211916472453,1,45824,179,0
        241211916809376,0,45824,179,0
        241211917486915,1,45824,179,0
        241211917815761,0,45824,179,0
        241211918424838,1,45824,179,0
        241211918650915,0,45824,179,0
        241211918760222,1,45824,179,0
        241211918973684,0,45824,179,0
        241211919810453,1,45824,179,0
        241211920094761,0,45824,179,0
        """))

  def test_linux_irqs(self):
    return DiffTestBlueprint(
        trace=DataPath('wattson_irq_gpu_markers.pb'),
        query="""
        INCLUDE PERFETTO MODULE linux.irqs;

        SELECT
          ts,
          MIN(dur) AS dur,
          name,
          id,
          parent_id,
          is_soft_irq
        FROM linux_irqs
        GROUP BY name
        """,
        out=Csv("""
        "ts","dur","name","id","parent_id","is_soft_irq"
        1701669861045,1994,"BLOCK",26750,"[NULL]",1
        1702703279625,4801,"IRQ (100a0000.BIG)",40985,"[NULL]",0
        1701729013999,1953,"IRQ (100a0000.LITTLE)",28512,"[NULL]",0
        1702778115806,2279,"IRQ (100a0000.MID)",42932,"[NULL]",0
        1702128463747,11800,"IRQ (10840000.pinctrl)",36938,"[NULL]",0
        1701651076906,4476,"IRQ (10970000.hsi2c)",26056,"[NULL]",0
        1703144502403,53263,"IRQ (176a0000.mbox)",48988,"[NULL]",0
        1701622901247,3255,"IRQ (1c0b0000.drmdpp)",25182,"[NULL]",0
        1702732122195,2644,"IRQ (1c0b1000.drmdpp)",41684,"[NULL]",0
        1702872227623,2604,"IRQ (1c0b2000.drmdpp)",44530,"[NULL]",0
        1702128740196,2807,"IRQ (1c0b3000.drmdpp)",36952,"[NULL]",0
        1701467062461,2848,"IRQ (1c0b4000.drmdpp)",21037,"[NULL]",0
        1701478183961,2767,"IRQ (1c0b5000.drmdpp)",21469,"[NULL]",0
        1701406364748,1871,"IRQ (1c2c0000.drmdsim)",19038,"[NULL]",0
        1702177862429,5533,"IRQ (1c300000.drmdecon)",37515,"[NULL]",0
        1703394103355,3662,"IRQ (1c500000.mali)",53392,"[NULL]",0
        1701692147056,122,"IRQ (IPI)",27439,"[NULL]",0
        1701693794557,2889,"IRQ (arm-pmu)",27499,"[NULL]",0
        1701648179242,1342,"IRQ (cs40l25a)",26000,"[NULL]",0
        1698403560059,1668,"IRQ (dwc3)",6740,"[NULL]",0
        1702128469566,4842,"IRQ (exynos-crtc-0)",36939,36938,0
        1701671403810,3215,"IRQ (exynos-mct)",26771,"[NULL]",0
        1702051098390,855,"IRQ (fts)",35709,"[NULL]",0
        1701667815309,5046,"IRQ (ufshcd)",26686,"[NULL]",0
        1701689581585,407,"RCU",27392,"[NULL]",1
        1703001573489,447,"SCHED",46634,"[NULL]",1
        1702673567426,1098,"TIMER",39947,"[NULL]",1
        """))
