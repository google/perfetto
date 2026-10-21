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

# Exercises battery state, wakeup reasons and inline (overflowed) history tags.
BATTERY_CHECKIN = """9,0,i,vers,36,214,AP1A.240305.019.A1,AP2A.240805.005.S4
9,hsp,0,0,"Abort:Pending Wakeup Sources: wlan"
9,hsp,1,10216,"com.android.chrome"
9,hsp,2,1000,"screen"
9,h,0:RESET:TIME:1732816430344
9,h,0,Bl=100,Bs=d,Bh=g,Bp=n,Bt=251,Bv=4395,Bcc=4263,Mrc=1.25,Wrc=0,+r,wr=0
9,h,0,Dcpu=100:50/10216:20:10
9,h,0,Dpst=10,20,30,40,50,60,
9,h,1000,Bt=263,Bh=h,Esc=2,-r
9,h,1000,Bh=c,Bt=-15,Bp=d,+r,wr=0:"Abort:a""b=c",Eds=2
9,h,1000,+Ejb=u0a52:"com.example/.Job",Xyz=1
9,h,1000,-Ejb=u0a52:"com.example/.Job",Bh=8,Wrc=0.5
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

  def test_standalone_battery_stats_checkin_battery_state(self):
    return DiffTestBlueprint(
        trace=RawText(BATTERY_CHECKIN),
        query="""
        SELECT
          name, unit, ts, value
        FROM counter
        JOIN counter_track ON counter.track_id = counter_track.id
        WHERE
          name IN (
            'BatteryTemperature',
            'BatteryHealth',
            'PlugType',
            'ModemRailCharge',
            'WifiRailCharge'
          )
        ORDER BY
          name, ts
        """,
        out=Csv("""
        "name","unit","ts","value"
        "BatteryHealth","[NULL]",1732816430344000000,2.000000
        "BatteryHealth","[NULL]",1732816431344000000,3.000000
        "BatteryHealth","[NULL]",1732816432344000000,7.000000
        "BatteryHealth","[NULL]",1732816434344000000,8.000000
        "BatteryTemperature","celsius",1732816430344000000,25.100000
        "BatteryTemperature","celsius",1732816431344000000,26.300000
        "BatteryTemperature","celsius",1732816432344000000,-1.500000
        "ModemRailCharge","mAh",1732816430344000000,1.250000
        "PlugType","[NULL]",1732816430344000000,0.000000
        "PlugType","[NULL]",1732816432344000000,8.000000
        "WifiRailCharge","mAh",1732816430344000000,0.000000
        "WifiRailCharge","mAh",1732816434344000000,0.500000
        """))

  def test_standalone_battery_stats_checkin_events(self):
    return DiffTestBlueprint(
        trace=RawText(BATTERY_CHECKIN),
        query="""
        SELECT
          ts, dur, track.name AS track, slice.name
        FROM slice
        JOIN track ON slice.track_id = track.id
        WHERE
          track.name GLOB 'battery_stats.*'
        ORDER BY
          ts, track.name
        """,
        out=Csv("""
        "ts","dur","track","name"
        1732816430344000000,0,"battery_stats.wakeup_reason","wakeup_reason=0:"Abort:Pending Wakeup Sources: wlan""
        1732816431344000000,0,"battery_stats.state","state=1000:"screen""
        1732816432344000000,0,"battery_stats.display_state_changed","display_state_changed=1000:"screen""
        1732816432344000000,0,"battery_stats.wakeup_reason","wakeup_reason=0:"Abort:a"b=c""
        1732816433344000000,0,"battery_stats.job","+job=10052:"com.example/.Job""
        1732816434344000000,0,"battery_stats.job","-job=10052:"com.example/.Job""
        """))

  def test_standalone_battery_stats_checkin_unhandled_items(self):
    return DiffTestBlueprint(
        trace=RawText(BATTERY_CHECKIN),
        query="""
        SELECT name, value
        FROM stats
        WHERE name = 'android_battery_stats_unhandled_items'
        """,
        out=Csv("""
        "name","value"
        "android_battery_stats_unhandled_items",1
        """))
