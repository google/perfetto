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
        1,186969441973689,186969489302704,47329015,"androidx.benchmark.integration.macrobenchmark.target","[NULL]"
        """))

  def test_warm_startups(self):
    return DiffTestBlueprint(
        trace=DataPath('api32_startup_warm.perfetto-trace'),
        query="""
        INCLUDE PERFETTO MODULE android.startup.startups;
        SELECT * FROM android_startups;
        """,
        out=Csv("""
        "startup_id","ts","ts_end","dur","package","startup_type"
        28,157479786566030,157479943081777,156515747,"androidx.benchmark.integration.macrobenchmark.target","[NULL]"
        """))

  def test_cold_startups(self):
    return DiffTestBlueprint(
        trace=DataPath('api34_startup_cold.perfetto-trace'),
        query="""
        INCLUDE PERFETTO MODULE android.startup.startups;
        SELECT * FROM android_startups;
        """,
        out=Csv("""
        "startup_id","ts","ts_end","dur","package","startup_type"
        61,17806781251694,17806891032171,109780477,"com.android.systemui.people","warm"
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
        1,779860286416,779893485322,33198906,"com.google.android.googlequicksearchbox","[NULL]"
        2,780778904571,780813944498,35039927,"androidx.benchmark.integration.macrobenchmark.target","[NULL]"
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
        1,799979565075,800014194731,34629656,"com.google.android.googlequicksearchbox","[NULL]"
        2,800868511677,800981929562,113417885,"androidx.benchmark.integration.macrobenchmark.target","[NULL]"
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
        1,791231114368,791501060868,269946500,"androidx.benchmark.integration.macrobenchmark.target","[NULL]"
        """))
