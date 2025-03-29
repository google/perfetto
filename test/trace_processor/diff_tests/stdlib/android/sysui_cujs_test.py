#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Cujs(TestSuite):

  def test_android_sysui_cujs(self):
    return DiffTestBlueprint(
        trace=Path('../../metrics/android/android_blocking_calls_cuj_per_frame_metric.py'),
        query="""
        INCLUDE PERFETTO MODULE android.cujs.sysui_cujs;
        SELECT *
        FROM sysui_cujs;
        """,
        out=Csv("""
        "cuj_id","upid","process_name","cuj_name","slice_id","ts","ts_end","dur","state","layer_id","begin_vsync","end_vsync","ui_thread"
        1,1,"com.android.systemui","BACK_PANEL_ARROW",4,10000000,67000000,57000000,"completed",0,20,30,3
        2,1,"com.android.systemui","BACK_PANEL_ARROW",24,85000000,91000000,6000000,"completed",2,60,70,3
        3,2,"com.google.android.apps.nexuslauncher","CUJ_NAME",38,104000000,144000000,40000000,"completed",1,80,90,5
        """))
