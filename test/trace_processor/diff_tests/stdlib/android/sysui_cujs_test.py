#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class SystemUICujs(TestSuite):

  def test_android_sysui_jank_cujs(self):
    return DiffTestBlueprint(
        trace=Path(
            '../../metrics/android/android_blocking_calls_cuj_per_frame_metric.py'
        ),
        query="""
        INCLUDE PERFETTO MODULE android.cujs.sysui_cujs;
        SELECT *
        FROM android_sysui_jank_cujs;
        """,
        out=Csv("""
        "cuj_id","upid","process_name","cuj_slice_name","cuj_name","slice_id","ts","ts_end","dur","state","ui_thread","layer_id","begin_vsync","end_vsync"
        1,1,"com.android.systemui","J<BACK_PANEL_ARROW>","BACK_PANEL_ARROW",4,27000000,65000000,38000000,"completed",3,0,20,30
        2,1,"com.android.systemui","J<BACK_PANEL_ARROW>","BACK_PANEL_ARROW",25,85000000,89000000,4000000,"completed",3,2,60,70
        3,2,"com.google.android.apps.nexuslauncher","J<CUJ_NAME>","CUJ_NAME",39,121000000,143000000,22000000,"completed",5,1,80,90
        """))

  def test_android_sysui_latency_cujs(self):
    return DiffTestBlueprint(
        trace=Path(
            '../../metrics/android/android_blocking_calls_cuj_per_frame_metric.py'
        ),
        query="""
        INCLUDE PERFETTO MODULE android.cujs.sysui_cujs;
        SELECT *
        FROM android_sysui_latency_cujs;
        """,
        out=Csv("""
        "cuj_id","upid","process_name","cuj_slice_name","cuj_name","slice_id","ts","ts_end","dur","state"
        1,1,"com.android.systemui","L<IGNORED_CUJ_1>","IGNORED_CUJ_1",53,150000000,155000000,5000000,"completed"
        2,1,"com.android.systemui","L<IGNORED_CUJ_2>","IGNORED_CUJ_2",58,156000000,160000000,4000000,"completed"
        """))
