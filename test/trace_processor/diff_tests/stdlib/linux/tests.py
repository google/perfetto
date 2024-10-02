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

        SELECT upid, utid, pid, tid, process_name, thread_name
        FROM linux_kernel_threads
        ORDER by utid LIMIT 10;
        """,
        out=Csv("""
        "upid","utid","pid","tid","process_name","thread_name"
        7,14,510,510,"sugov:0","sugov:0"
        89,23,1365,1365,"com.google.usf.","com.google.usf."
        87,37,1249,1249,"irq/357-dwc3","irq/357-dwc3"
        31,38,6,6,"kworker/u16:0","kworker/u16:0"
        11,42,511,511,"sugov:4","sugov:4"
        83,43,1152,1152,"irq/502-fts_ts","irq/502-fts_ts"
        93,44,2374,2374,"csf_sync_update","csf_sync_update"
        18,45,2379,2379,"csf_kcpu_0","csf_kcpu_0"
        12,47,247,247,"decon0_kthread","decon0_kthread"
        65,48,159,159,"spi0","spi0"
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
