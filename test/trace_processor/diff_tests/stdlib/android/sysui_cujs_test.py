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
        2,1,"com.android.systemui","J<BACK_PANEL_ARROW>","BACK_PANEL_ARROW",31,85000000,89000000,4000000,"completed",3,2,60,70
        3,2,"com.google.android.apps.nexuslauncher","J<CUJ_NAME>","CUJ_NAME",45,121000000,143000000,22000000,"completed",7,1,80,90
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
        1,1,"com.android.systemui","L<IGNORED_CUJ_1>","IGNORED_CUJ_1",60,150000000,155000000,5000000,"completed"
        2,1,"com.android.systemui","L<IGNORED_CUJ_2>","IGNORED_CUJ_2",65,156000000,160000000,4000000,"completed"
        """))

  def test_android_jank_latency_cujs(self):
    return DiffTestBlueprint(
        trace=Path(
            '../../metrics/android/android_blocking_calls_cuj_per_frame_metric.py'
        ),
        query="""
        INCLUDE PERFETTO MODULE android.cujs.sysui_cujs;
        SELECT *
        FROM android_jank_latency_cujs;
        """,
        out=Csv("""
        "cuj_id","id","upid","process_name","cuj_slice_name","cuj_name","slice_id","ts","ts_end","dur","state","ui_thread","layer_id","begin_vsync","end_vsync","cuj_type"
        1,1,1,"com.android.systemui","J<BACK_PANEL_ARROW>","BACK_PANEL_ARROW",4,27000000,65000000,38000000,"completed",3,0,20,30,"jank"
        2,2,1,"com.android.systemui","J<BACK_PANEL_ARROW>","BACK_PANEL_ARROW",31,85000000,89000000,4000000,"completed",3,2,60,70,"jank"
        3,3,2,"com.google.android.apps.nexuslauncher","J<CUJ_NAME>","CUJ_NAME",45,121000000,143000000,22000000,"completed",7,1,80,90,"jank"
        1,1,1,"com.android.systemui","L<IGNORED_CUJ_1>","IGNORED_CUJ_1",60,150000000,155000000,5000000,"completed",1,"[NULL]","[NULL]","[NULL]","latency"
        2,2,1,"com.android.systemui","L<IGNORED_CUJ_2>","IGNORED_CUJ_2",65,156000000,160000000,4000000,"completed",1,"[NULL]","[NULL]","[NULL]","latency"
        """))

  def test_android_sysui_latency_cujs_state(self):
    return DiffTestBlueprint(
        trace=Path('latency_cujs_state_trace.py'),
        query="""
        INCLUDE PERFETTO MODULE android.cujs.sysui_cujs;
        SELECT cuj_name, state
        FROM android_sysui_latency_cujs
        ORDER BY cuj_name;
        """,
        out=Csv("""
        "cuj_name","state"
        "CUJ_CANCELED","canceled"
        "CUJ_COMPLETED","completed"
        "CUJ_TIMEOUT","timeout"
        """))

  def test_android_cuj_blocking_calls(self):
    return DiffTestBlueprint(
        trace=Path(
            '../../metrics/android/android_blocking_calls_cuj_per_frame_metric.py'
        ),
        query="""
        INCLUDE PERFETTO MODULE android.cujs.sysui_cujs;
        SELECT *
        FROM android_cuj_blocking_calls
        ORDER BY cuj_id, cuj_type, name, ts;
        """,
        out=Csv("""
        "slice_id","name","ts","dur","ts_end","cuj_id","cuj_name","process_name","upid","utid","cuj_type"
        13,"CreateGraphicsPipeline",27100000,800000,27900000,1,"BACK_PANEL_ARROW","com.android.systemui",1,4,"jank"
        11,"DrawFrames",27000000,1000000,28000000,1,"BACK_PANEL_ARROW","com.android.systemui",1,4,"jank"
        22,"DrawFrames",44000000,1000000,45000000,1,"BACK_PANEL_ARROW","com.android.systemui",1,4,"jank"
        26,"DrawFrames",61000000,1000000,62000000,1,"BACK_PANEL_ARROW","com.android.systemui",1,4,"jank"
        15,"Texture upload",27200000,100000,27300000,1,"BACK_PANEL_ARROW","com.android.systemui",1,4,"jank"
        20,"animation",30000000,2000000,32000000,1,"BACK_PANEL_ARROW","com.android.systemui",1,3,"jank"
        29,"animation",62000000,2000000,64000000,1,"BACK_PANEL_ARROW","com.android.systemui",1,3,"jank"
        12,"binder transaction",27000000,500000,27500000,1,"BACK_PANEL_ARROW","com.android.systemui",1,3,"jank"
        18,"binder transaction",27500000,2500000,30000000,1,"BACK_PANEL_ARROW","com.android.systemui",1,3,"jank"
        14,"drawLayer",27100000,100000,27200000,1,"BACK_PANEL_ARROW","com.android.systemui",1,4,"jank"
        14,"drawLayer [TestLayer]",27100000,100000,27200000,1,"BACK_PANEL_ARROW","com.android.systemui",1,4,"jank"
        17,"flush commands",27400000,100000,27500000,1,"BACK_PANEL_ARROW","com.android.systemui",1,4,"jank"
        16,"flush layers",27300000,100000,27400000,1,"BACK_PANEL_ARROW","com.android.systemui",1,4,"jank"
        19,"queueBuffer",27500000,100000,27600000,1,"BACK_PANEL_ARROW","com.android.systemui",1,4,"jank"
        36,"DrawFrames",85000000,1000000,86000000,2,"BACK_PANEL_ARROW","com.android.systemui",1,4,"jank"
        39,"binder transaction",86000000,2500000,88500000,2,"BACK_PANEL_ARROW","com.android.systemui",1,3,"jank"
        50,"DrawFrames",121000000,800000,121800000,3,"CUJ_NAME","com.google.android.apps.nexuslauncher",2,6,"jank"
        58,"DrawFrames",142000000,500000,142500000,3,"CUJ_NAME","com.google.android.apps.nexuslauncher",2,6,"jank"
        54,"animation",127000000,11000000,138000000,3,"CUJ_NAME","com.google.android.apps.nexuslauncher",2,7,"jank"
        """))
