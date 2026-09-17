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
        SELECT
          process_name,
          cuj_slice_name,
          cuj_name,
          ts,
          ts_end,
          dur,
          state,
          begin_vsync,
          end_vsync,
          cuj_type
        FROM android_jank_latency_cujs;
        """,
        out=Csv("""
        "process_name","cuj_slice_name","cuj_name","ts","ts_end","dur","state","begin_vsync","end_vsync","cuj_type"
        "com.android.systemui","J<BACK_PANEL_ARROW>","BACK_PANEL_ARROW",27000000,65000000,38000000,"completed",20,30,"jank"
        "com.android.systemui","L<IGNORED_CUJ_1>","IGNORED_CUJ_1",150000000,155000000,5000000,"completed","[NULL]","[NULL]","latency"
        "com.android.systemui","J<BACK_PANEL_ARROW>","BACK_PANEL_ARROW",85000000,89000000,4000000,"completed",60,70,"jank"
        "com.android.systemui","L<IGNORED_CUJ_2>","IGNORED_CUJ_2",156000000,160000000,4000000,"completed","[NULL]","[NULL]","latency"
        "com.google.android.apps.nexuslauncher","J<CUJ_NAME>","CUJ_NAME",121000000,143000000,22000000,"completed",80,90,"jank"
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
        SELECT name, ts, dur, ts_end, cuj_name, process_name, cuj_type
        FROM android_cuj_blocking_calls
        ORDER BY ts;
        """,
        out=Csv("""
        "name","ts","dur","ts_end","cuj_name","process_name","cuj_type"
        "binder transaction",27000000,500000,27500000,"BACK_PANEL_ARROW","com.android.systemui","jank"
        "DrawFrames",27000000,1000000,28000000,"BACK_PANEL_ARROW","com.android.systemui","jank"
        "CreateGraphicsPipeline",27100000,800000,27900000,"BACK_PANEL_ARROW","com.android.systemui","jank"
        "drawLayer [TestLayer]",27100000,100000,27200000,"BACK_PANEL_ARROW","com.android.systemui","jank"
        "drawLayer",27100000,100000,27200000,"BACK_PANEL_ARROW","com.android.systemui","jank"
        "Texture upload",27200000,100000,27300000,"BACK_PANEL_ARROW","com.android.systemui","jank"
        "flush layers",27300000,100000,27400000,"BACK_PANEL_ARROW","com.android.systemui","jank"
        "flush commands",27400000,100000,27500000,"BACK_PANEL_ARROW","com.android.systemui","jank"
        "binder transaction",27500000,2500000,30000000,"BACK_PANEL_ARROW","com.android.systemui","jank"
        "queueBuffer",27500000,100000,27600000,"BACK_PANEL_ARROW","com.android.systemui","jank"
        "animation",30000000,2000000,32000000,"BACK_PANEL_ARROW","com.android.systemui","jank"
        "DrawFrames",44000000,1000000,45000000,"BACK_PANEL_ARROW","com.android.systemui","jank"
        "DrawFrames",61000000,1000000,62000000,"BACK_PANEL_ARROW","com.android.systemui","jank"
        "animation",62000000,2000000,64000000,"BACK_PANEL_ARROW","com.android.systemui","jank"
        "DrawFrames",85000000,1000000,86000000,"BACK_PANEL_ARROW","com.android.systemui","jank"
        "binder transaction",86000000,2500000,88500000,"BACK_PANEL_ARROW","com.android.systemui","jank"
        "DrawFrames",121000000,800000,121800000,"CUJ_NAME","com.google.android.apps.nexuslauncher","jank"
        "animation",127000000,11000000,138000000,"CUJ_NAME","com.google.android.apps.nexuslauncher","jank"
        "DrawFrames",142000000,500000,142500000,"CUJ_NAME","com.google.android.apps.nexuslauncher","jank"
        """))
