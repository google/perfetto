# Copyright (C) 2026 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, DiffTestBlueprint, TestSuite, TextProto, Json, Path


class Freezer(TestSuite):

  def test_freezer_state_statsd_intervals(self):
    return DiffTestBlueprint(
        trace=Path('freezer_data.py'),
        query="""
            INCLUDE PERFETTO MODULE android.freezer;
            SELECT ts, dur, pid, process_name, freezer_state, time_unfrozen_millis, unfreeze_reason
            FROM android_freezer_state_statsd;
            """,
        out=Csv("""
            "ts","dur","pid","process_name","freezer_state","time_unfrozen_millis","unfreeze_reason"
            1000000000,4000000000,1234,"com.example.app","FREEZE_APP",0,"UFR_NONE"
            5000000000,0,1234,"com.example.app","UNFREEZE_APP",4,"UFR_ACTIVITY"
            """))

  def test_android_freezer_intervals(self):
    return DiffTestBlueprint(
        trace=Path('freezer_intervals.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.freezer;
        SELECT ts, dur, pid, state
        FROM _android_freezer_intervals
        ORDER BY ts, pid;
        """,
        out=Csv("""
        "ts","dur","pid","state"
        500,-1,400,"UNFROZEN"
        1000,2000,200,"UNFROZEN"
        3000,2000,200,"FROZEN"
        5000,1000,200,"UNFROZEN"
        6000,2000,200,"FROZEN"
        7000,-1,300,"FROZEN"
        """))
