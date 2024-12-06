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

from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


EXAMPLE_CHECKIN = """9,0,i,vers,36,214,AP1A.240305.019.A1,AP2A.240805.005.S4
9,hsp,0,10216,"com.android.chrome"
9,h,0:RESET:TIME:1732816430344
9,h,0,Bl=100,Bs=d,Bh=g,Bp=n,Bt=251,Bv=4395,Bcc=4263,Mrc=0,Wrc=0,+r,+w,+s,+Wr,+S,+BP,Pcn=lte,Pss=3,+W,Wss=4,Wsp=compl,+Chtp,Gss=none,nrs=1,Etp=0
"""


class AndroidDumpstate(TestSuite):
  def test_android_dumpstate_standalone_battery_stats_checkin(self):
    return DiffTestBlueprint(
        trace=Csv(EXAMPLE_CHECKIN),
        query="""
        SELECT
          name, ts, value
        FROM counter
        JOIN counter_track ON counter.track_id = counter_track.id
        WHERE
          name = 'ScreenState'
          AND classification = 'screen_state'
        """,
        out=Csv("""
        "name","ts","value"
        "ScreenState",1732816430344000000,2.000000
        """))
