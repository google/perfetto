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


class SmokeSchedEvents(TestSuite):
  # Contains smoke tests which test the most fundamentally important features
  # trace processor  Note: new tests here should only be added by the Perfetto
  # Sched events
  def test_android_sched_and_ps_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('android_sched_and_ps.pb'),
        query="""
        SELECT
          ts,
          cpu,
          dur,
          end_state,
          priority,
          tid
        FROM sched
        JOIN thread USING(utid)
        ORDER BY ts
        LIMIT 10;
        """,
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

  # Sched events from sythetic trace
  def test_synth_1_smoke(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query="""
        SELECT
          ts,
          cpu,
          dur,
          end_state,
          priority,
          tid
        FROM sched
        JOIN thread USING(utid)
        ORDER BY ts
        LIMIT 10;
        """,
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
