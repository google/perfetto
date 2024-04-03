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


class CpuStdlib(TestSuite):
  # Test CPU frequency counter grouping.
  def test_cpu_eos_counters_freq(self):
    return DiffTestBlueprint(
        trace=DataPath('android_cpu_eos.pb'),
        query=("""
             INCLUDE PERFETTO MODULE cpu.freq;
             select
               track_id,
               freq,
               cpu,
               sum(dur) as dur
             from cpu_freq_counters
             GROUP BY freq, cpu
             """),
        out=Csv("""
            "track_id","freq","cpu","dur"
            33,614400,0,4755967239
            34,614400,1,4755971561
            35,614400,2,4755968228
            36,614400,3,4755964320
            33,864000,0,442371195
            34,864000,1,442397134
            35,864000,2,442417916
            36,864000,3,442434530
            33,1363200,0,897122398
            34,1363200,1,897144167
            35,1363200,2,897180154
            36,1363200,3,897216772
            33,1708800,0,2553979530
            34,1708800,1,2553923073
            35,1708800,2,2553866772
            36,1708800,3,2553814688
            """))

  # Test CPU idle state counter grouping.
  def test_cpu_eos_counters_idle(self):
    return DiffTestBlueprint(
        trace=DataPath('android_cpu_eos.pb'),
        query=("""
             INCLUDE PERFETTO MODULE cpu.idle;
             select
               track_id,
               idle,
               cpu,
               sum(dur) as dur
             from cpu_idle_counters
             GROUP BY idle, cpu
             """),
        out=Csv("""
             "track_id","idle","cpu","dur"
             0,-1,0,2839828332
             37,-1,1,1977033843
             32,-1,2,1800498713
             1,-1,3,1884366297
             0,0,0,1833971336
             37,0,1,2285260950
             32,0,2,1348416182
             1,0,3,1338508968
             0,1,0,4013820433
             37,1,1,4386917600
             32,1,2,5532102915
             1,1,3,5462026920
            """))
