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


class Memory(TestSuite):

  def test_memory_rss_and_swap_per_process(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE memory.linux.process;

        SELECT *
        FROM memory_rss_and_swap_per_process
        WHERE upid = 1
        LIMIT 5
        """,
        out=Csv("""
        "ts","dur","upid","pid","process_name","anon_rss","file_rss","shmem_rss","rss","swap","anon_rss_and_swap","rss_and_swap"
        37592474220,12993896,1,1982,"com.android.systemui",125865984,"[NULL]","[NULL]","[NULL]","[NULL]",125865984,"[NULL]"
        37605468116,1628,1,1982,"com.android.systemui",126050304,"[NULL]","[NULL]","[NULL]","[NULL]",126050304,"[NULL]"
        37605469744,1302,1,1982,"com.android.systemui",126050304,"[NULL]",2990080,"[NULL]","[NULL]",126050304,"[NULL]"
        37605471046,685791,1,1982,"com.android.systemui",126046208,"[NULL]",2990080,"[NULL]","[NULL]",126046208,"[NULL]"
        37606156837,6510,1,1982,"com.android.systemui",126042112,"[NULL]",2990080,"[NULL]","[NULL]",126042112,"[NULL]"
            """))

