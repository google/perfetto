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

from python.generators.diff_tests.testing import Path, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Smoke(DiffTestModule):

  def test_sfgate_smoke(self):
    return DiffTestBlueprint(
        trace=Path('../../data/sfgate.json'),
        query=Path('../common/smoke_test.sql'),
        out=Csv("""
"ts","cpu","dur","end_state","priority","tid"
"""))

  def test_sfgate_smoke_slices(self):
    return DiffTestBlueprint(
        trace=Path('../../data/sfgate.json'),
        query=Path('../common/smoke_slices_test.sql'),
        out=Csv("""
"type","depth","count"
"thread_track",0,16888
"thread_track",1,19447
"thread_track",2,5816
"thread_track",3,829
"thread_track",4,191
"thread_track",5,94
"thread_track",6,57
"thread_track",7,19
"thread_track",8,14
"thread_track",9,2
"""))

  def test_android_sched_and_ps_smoke(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query=Path('../common/smoke_test.sql'),
        out=Csv("""
"ts","cpu","dur","end_state","priority","tid"
81473010031230,2,78021,"S",120,26204
81473010109251,2,12500,"R",120,0
81473010121751,2,58021,"S",120,26205
81473010179772,2,24114,"R",120,0
81473010203886,2,30834,"S",120,26206
81473010234720,2,43802,"R",120,0
81473010278522,2,29948,"S",120,26207
81473010308470,2,44322,"R",120,0
81473010341386,1,158854,"S",116,23912
81473010352792,2,32917,"S",120,26208
"""))

  def test_compressed_smoke(self):
    return DiffTestBlueprint(
        trace=Path('../../data/compressed.pb'),
        query=Path('../common/smoke_test.sql'),
        out=Csv("""
"ts","cpu","dur","end_state","priority","tid"
170601497673450,2,53646,"DK",120,6790
170601497691210,7,22917,"R",120,0
170601497714127,7,29167,"D",120,6732
170601497727096,2,55156,"S",120,62
170601497743294,7,862656,"R",120,0
170601497766106,3,13594,"S",120,8
170601497779700,3,31094,"D",120,6790
170601497782252,2,875313,"R",120,0
170601497810794,3,824635,"R",120,0
170601498605950,7,158333,"D",120,6732
"""))

  def test_synth_1_smoke(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('../common/smoke_test.sql'),
        out=Csv("""
"ts","cpu","dur","end_state","priority","tid"
1,0,99,"R",0,3
50,1,70,"R",0,1
100,0,15,"R",0,2
115,0,-1,"[NULL]",0,3
120,1,50,"R",0,2
170,1,80,"R",0,0
250,1,140,"R",0,2
390,1,-1,"[NULL]",0,4
"""))

  def test_thread_cpu_time_example_android_trace_30s(self):
    return DiffTestBlueprint(
        trace=Path('../../data/example_android_trace_30s.pb'),
        query=Path('thread_cpu_time_test.sql'),
        out=Path('thread_cpu_time_example_android_trace_30s.out'))

  def test_proxy_power(self):
    return DiffTestBlueprint(
        trace=Path('../../data/cpu_counters.pb'),
        query=Path('proxy_power_test.sql'),
        out=Path('proxy_power.out'))
