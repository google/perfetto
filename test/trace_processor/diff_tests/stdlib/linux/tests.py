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