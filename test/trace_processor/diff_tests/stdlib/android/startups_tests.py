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

from python.generators.diff_tests.testing import Path, DataPath
from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Startups(TestSuite):

  def test_hot_startups(self):
    return DiffTestBlueprint(
        trace=DataPath('api31_startup_hot.perfetto-trace'),
        query="""
        INCLUDE PERFETTO MODULE android.startup.startups;
        SELECT * FROM android_startups;
        """,
        out=Csv("""
        "startup_id","ts","ts_end","dur","package","startup_type"
        0,186969441973689,186969489302704,47329015,"androidx.benchmark.integration.macrobenchmark.target","[NULL]"
        """))

  def test_warm_startups(self):
    return DiffTestBlueprint(
        trace=DataPath('api31_startup_warm.perfetto-trace'),
        query="""
        INCLUDE PERFETTO MODULE android.startup.startups;
        SELECT * FROM android_startups;
        """,
        out=Csv("""
        "startup_id","ts","ts_end","dur","package","startup_type"
        0,186982050780778,186982115528805,64748027,"androidx.benchmark.integration.macrobenchmark.target","[NULL]"
        """))

  def test_cold_startups(self):
    return DiffTestBlueprint(
        trace=DataPath('api31_startup_cold.perfetto-trace'),
        query="""
        INCLUDE PERFETTO MODULE android.startup.startups;
        SELECT * FROM android_startups;
        """,
        out=Csv("""
        "startup_id","ts","ts_end","dur","package","startup_type"
        0,186974938196632,186975083989042,145792410,"androidx.benchmark.integration.macrobenchmark.target","[NULL]"
        """))

  def test_hot_startups_maxsdk28(self):
    return DiffTestBlueprint(
        trace=DataPath('api24_startup_hot.perfetto-trace'),
        query="""
        INCLUDE PERFETTO MODULE android.startup.startups;
        SELECT * FROM android_startups;
        """,
        out=Csv("""
        "startup_id","ts","ts_end","dur","package","startup_type"
        0,779860286416,779893485322,33198906,"com.google.android.googlequicksearchbox","hot"
        1,780778904571,780813944498,35039927,"androidx.benchmark.integration.macrobenchmark.target","hot"
        """))

  def test_warm_startups_maxsdk28(self):
    return DiffTestBlueprint(
        trace=DataPath('api24_startup_warm.perfetto-trace'),
        query="""
        INCLUDE PERFETTO MODULE android.startup.startups;
        SELECT * FROM android_startups;
        """,
        out=Csv("""
        "startup_id","ts","ts_end","dur","package","startup_type"
        0,799979565075,800014194731,34629656,"com.google.android.googlequicksearchbox","hot"
        1,800868511677,800981929562,113417885,"androidx.benchmark.integration.macrobenchmark.target","[NULL]"
        """))

  def test_cold_startups_maxsdk28(self):
    return DiffTestBlueprint(
        trace=DataPath('api24_startup_cold.perfetto-trace'),
        query="""
        INCLUDE PERFETTO MODULE android.startup.startups;
        SELECT * FROM android_startups;
        """,
        out=Csv("""
        "startup_id","ts","ts_end","dur","package","startup_type"
        0,791231114368,791501060868,269946500,"androidx.benchmark.integration.macrobenchmark.target","[NULL]"
        """))

  def test_android_startup_time_to_display_hot_maxsdk28(self):
    return DiffTestBlueprint(
        trace=DataPath('api24_startup_hot.perfetto-trace'),
        query="""
        INCLUDE PERFETTO MODULE android.startup.time_to_display;
        SELECT * FROM android_startup_time_to_display;
        """,
        out=Csv("""
        "startup_id","time_to_initial_display","time_to_full_display","ttid_frame_id","ttfd_frame_id","upid"
        0,33198906,"[NULL]",1,"[NULL]",355
        1,35039927,537343160,4,5,383
        """))

  def test_android_startup_time_to_display_warm_maxsdk28(self):
    return DiffTestBlueprint(
        trace=DataPath('api24_startup_warm.perfetto-trace'),
        query="""
        INCLUDE PERFETTO MODULE android.startup.time_to_display;
        SELECT * FROM android_startup_time_to_display;
        """,
        out=Csv("""
        "startup_id","time_to_initial_display","time_to_full_display","ttid_frame_id","ttfd_frame_id","upid"
        0,34629656,"[NULL]",1,"[NULL]",355
        1,108563770,581026583,4,5,388
        """))

  def test_android_startup_time_to_display_cold_maxsdk28(self):
    return DiffTestBlueprint(
        trace=DataPath('api24_startup_cold.perfetto-trace'),
        query="""
        INCLUDE PERFETTO MODULE android.startup.time_to_display;
        SELECT * FROM android_startup_time_to_display;
        """,
        out=Csv("""
        "startup_id","time_to_initial_display","time_to_full_display","ttid_frame_id","ttfd_frame_id","upid"
        0,264869885,715406822,65,66,396
        """))

  def test_android_startup_time_to_display_hot(self):
    return DiffTestBlueprint(
        trace=DataPath('api31_startup_hot.perfetto-trace'),
        query="""
        INCLUDE PERFETTO MODULE android.startup.time_to_display;
        SELECT * FROM android_startup_time_to_display;
        """,
        out=Csv("""
        "startup_id","time_to_initial_display","time_to_full_display","ttid_frame_id","ttfd_frame_id","upid"
        0,40534066,542222554,5872867,5872953,184
        """))

  def test_android_startup_time_to_display_warm(self):
    return DiffTestBlueprint(
        trace=DataPath('api31_startup_warm.perfetto-trace'),
        query="""
        INCLUDE PERFETTO MODULE android.startup.time_to_display;
        SELECT * FROM android_startup_time_to_display;
        """,
        out=Csv("""
        "startup_id","time_to_initial_display","time_to_full_display","ttid_frame_id","ttfd_frame_id","upid"
        0,62373965,555968701,5873800,5873889,185
        """))

  def test_android_startup_time_to_display_cold(self):
    return DiffTestBlueprint(
        trace=DataPath('api31_startup_cold.perfetto-trace'),
        query="""
        INCLUDE PERFETTO MODULE android.startup.time_to_display;
        SELECT * FROM android_startup_time_to_display;
        """,
        out=Csv("""
        "startup_id","time_to_initial_display","time_to_full_display","ttid_frame_id","ttfd_frame_id","upid"
        0,143980066,620815843,5873276,5873353,229
        """))
