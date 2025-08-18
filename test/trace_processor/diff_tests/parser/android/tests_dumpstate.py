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

from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import RawText
from python.generators.diff_tests.testing import TestSuite

EXAMPLE_CHECKIN = """9,0,i,vers,36,214,AP1A.240305.019.A1,AP2A.240805.005.S4
9,hsp,0,10216,"com.android.chrome"
9,hsp,1,1001,"*telephony-radio*"
9,hsp,2,10238,"*alarm*"
9,hsp,3,1002,"*telephony-radio*"
9,h,0:RESET:TIME:1732816430344
9,h,0,Bl=100,Bs=d,Bh=g,Bp=n,Bt=251,Bv=4395,Bcc=4263,Mrc=0,Wrc=0,+r,+w,+s,+Wr,+S,+BP,Pcn=lte,Pss=3,+W,Wss=4,Wsp=compl,+Chtp,Gss=none,nrs=1,Etp=0
9,h,10,+w=1
9,h,10,-w=1
9,h,10,+w=1
9,h,10,+w=2
9,h,10,-w=1
9,h,10,-w=2
9,h,10,+w=1
9,h,10,+w=3
9,h,10,-w=1
9,h,10,-w=3
"""


class AndroidDumpstate(TestSuite):

  def test_dumpstate_trivial_trace(self):
    return DiffTestBlueprint(
        trace=RawText(
            "========================================================\n"
            "== dumpstate: 2021-08-24 23:35:40\n"
            "========================================================\n"
            "\n"
            "Build: crosshatch-userdebug 12 SPB5.210812.002 7671067 dev-keys\n"
            "Build fingerprint: 'google/crosshatch/crosshatch:12/SPB5.210812.002/7671067:userdebug/dev-keys'\n"
        ),
        query="""
        SELECT
          section, service, line
        FROM android_dumpstate
        ORDER BY id
        """,
        out=Csv("""
        "section","service","line"
        "[NULL]","[NULL]","========================================================"
        "[NULL]","[NULL]","== dumpstate: 2021-08-24 23:35:40"
        "[NULL]","[NULL]","========================================================"
        "[NULL]","[NULL]",""
        "[NULL]","[NULL]","Build: crosshatch-userdebug 12 SPB5.210812.002 7671067 dev-keys"
        "[NULL]","[NULL]","Build fingerprint: 'google/crosshatch/crosshatch:12/SPB5.210812.002/7671067:userdebug/dev-keys'"
        """))

  def test_android_dumpstate_standalone_battery_stats_checkin(self):
    return DiffTestBlueprint(
        trace=RawText(EXAMPLE_CHECKIN),
        query="""
        SELECT
          name, ts, value
        FROM counter
        JOIN counter_track ON counter.track_id = counter_track.id
        WHERE
          name = 'ScreenState'
          AND type = 'screen_state'
        """,
        out=Csv("""
        "name","ts","value"
        "ScreenState",1732816430344000000,2.000000
        """))

  def test_standalone_battery_stats_checkin_wakelocks(self):
    return DiffTestBlueprint(
        trace=RawText(EXAMPLE_CHECKIN),
        query="""
        SELECT
          ts, slice.name, dur
        FROM slice
        JOIN track ON slice.track_id = track.id
        WHERE
          track.name = 'WakeLocks'
        ORDER BY
          ts, slice.name
        """,
        out=Csv("""
        "ts","name","dur"
        1732816430354000000,"*telephony-radio*",10000000
        1732816430374000000,"*telephony-radio*",20000000
        1732816430384000000,"*alarm*",20000000
        1732816430414000000,"*telephony-radio*",20000000
        1732816430424000000,"*telephony-radio*",20000000
        """))
