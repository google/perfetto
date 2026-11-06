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

  def test_android_jank_latency_cujs_latency_ui_thread(self):
    # In this trace, the upid of SystemUI is not the utid of its main thread.
    return DiffTestBlueprint(
        trace=Path('latency_cuj_blocking_calls_trace.py'),
        query="""
        INCLUDE PERFETTO MODULE android.cujs.sysui_cujs;
        SELECT cuj.cuj_name, cuj.cuj_type, thread.tid AS ui_thread_tid
        FROM android_jank_latency_cujs AS cuj
        LEFT JOIN thread ON thread.utid = cuj.ui_thread;
        """,
        out=Csv("""
        "cuj_name","cuj_type","ui_thread_tid"
        "ACTION_EXPAND_PANEL","latency",1000
        """))

  def test_android_cuj_blocking_calls_latency_cuj_main_thread(self):
    # Blocking calls on the main thread and on every RenderThread count for
    # latency CUJs; calls on other threads do not. Main-thread calls must appear
    # once even when the process has several RenderThreads.
    return DiffTestBlueprint(
        trace=Path('latency_cuj_blocking_calls_trace.py'),
        query="""
        INCLUDE PERFETTO MODULE android.cujs.sysui_cujs;
        SELECT bc.name, bc.ts, bc.dur, bc.cuj_name, bc.cuj_type, thread.tid
        FROM android_cuj_blocking_calls AS bc
        JOIN thread USING (utid)
        ORDER BY bc.ts;
        """,
        out=Csv("""
        "name","ts","dur","cuj_name","cuj_type","tid"
        "measure",12000000,2000000,"ACTION_EXPAND_PANEL","latency",1000
        "layout",15000000,3000000,"ACTION_EXPAND_PANEL","latency",1000
        "CreateGraphicsPipeline",19000000,1000000,"ACTION_EXPAND_PANEL","latency",1500
        "flush layers",20000000,500000,"ACTION_EXPAND_PANEL","latency",1501
        """))

  def test_android_cuj_frame_boundaries(self):
    return DiffTestBlueprint(
        trace=Path(
            '../../metrics/android/android_blocking_calls_cuj_per_frame_metric.py'
        ),
        query="""
        INCLUDE PERFETTO MODULE android.frame_blocking_calls.blocking_calls_aggregation;
        SELECT
          cuj_id,
          cuj_name,
          upid,
          process_name,
          frame_id,
          layer_id,
          ts,
          ts_end,
          dur
        FROM android_cuj_frame_boundaries;
        """,
        out=Csv("""
        "cuj_id","cuj_name","upid","process_name","frame_id","layer_id","ts","ts_end","dur"
        1,"BACK_PANEL_ARROW",1,"com.android.systemui",20,0,27000000,43000000,16000000
        1,"BACK_PANEL_ARROW",1,"com.android.systemui",22,0,44000000,60000000,16000000
        1,"BACK_PANEL_ARROW",1,"com.android.systemui",24,0,61000000,67000000,6000000
        2,"BACK_PANEL_ARROW",1,"com.android.systemui",65,2,85000000,91000000,6000000
        3,"CUJ_NAME",2,"com.google.android.apps.nexuslauncher",80,1,121000000,139000000,18000000
        3,"CUJ_NAME",2,"com.google.android.apps.nexuslauncher",82,1,139000000,145000000,6000000
        """))

  def test_android_cuj_frame_blocking_calls(self):
    return DiffTestBlueprint(
        trace=Path(
            '../../metrics/android/android_blocking_calls_cuj_per_frame_metric.py'
        ),
        query="""
        INCLUDE PERFETTO MODULE android.frame_blocking_calls.blocking_calls_aggregation;
        SELECT
          id,
          call_id,
          name,
          ts,
          dur,
          ts_end,
          cuj_id,
          cuj_name,
          frame_id,
          process_name
        FROM android_cuj_frame_blocking_calls;
        """,
        out=Csv("""
        "id","call_id","name","ts","dur","ts_end","cuj_id","cuj_name","frame_id","process_name"
        1,5,"binder transaction",27000000,500000,27500000,1,"BACK_PANEL_ARROW",20,"com.android.systemui"
        2,2,"DrawFrames",27000000,1000000,28000000,1,"BACK_PANEL_ARROW",20,"com.android.systemui"
        3,7,"drawLayer",27100000,100000,27200000,1,"BACK_PANEL_ARROW",20,"com.android.systemui"
        4,8,"drawLayer [TestLayer]",27100000,100000,27200000,1,"BACK_PANEL_ARROW",20,"com.android.systemui"
        5,1,"CreateGraphicsPipeline",27100000,800000,27900000,1,"BACK_PANEL_ARROW",20,"com.android.systemui"
        6,3,"Texture upload",27200000,100000,27300000,1,"BACK_PANEL_ARROW",20,"com.android.systemui"
        7,10,"flush layers",27300000,100000,27400000,1,"BACK_PANEL_ARROW",20,"com.android.systemui"
        8,9,"flush commands",27400000,100000,27500000,1,"BACK_PANEL_ARROW",20,"com.android.systemui"
        9,11,"hwuiTask",27400000,200000,27600000,1,"BACK_PANEL_ARROW",20,"com.android.systemui"
        10,12,"queueBuffer",27500000,100000,27600000,1,"BACK_PANEL_ARROW",20,"com.android.systemui"
        11,6,"binder transaction",27500000,2500000,30000000,1,"BACK_PANEL_ARROW",20,"com.android.systemui"
        12,4,"animation",30000000,2000000,32000000,1,"BACK_PANEL_ARROW",20,"com.android.systemui"
        13,13,"DrawFrames",44000000,1000000,45000000,1,"BACK_PANEL_ARROW",22,"com.android.systemui"
        14,14,"DrawFrames",61000000,1000000,62000000,1,"BACK_PANEL_ARROW",24,"com.android.systemui"
        15,15,"animation",62000000,2000000,64000000,1,"BACK_PANEL_ARROW",24,"com.android.systemui"
        16,16,"DrawFrames",85000000,1000000,86000000,2,"BACK_PANEL_ARROW",65,"com.android.systemui"
        17,17,"binder transaction",86000000,2500000,88500000,2,"BACK_PANEL_ARROW",65,"com.android.systemui"
        18,18,"DrawFrames",121000000,800000,121800000,3,"CUJ_NAME",80,"com.google.android.apps.nexuslauncher"
        19,19,"animation",127000000,11000000,138000000,3,"CUJ_NAME",80,"com.google.android.apps.nexuslauncher"
        20,20,"DrawFrames",142000000,500000,142500000,3,"CUJ_NAME",82,"com.google.android.apps.nexuslauncher"
        """))

  def test_android_cuj_frame_blocking_calls_hwui_and_canceled_cuj(self):
    return DiffTestBlueprint(
        trace=Path('cuj_frame_blocking_calls_trace.py'),
        query="""
        INCLUDE PERFETTO MODULE android.frame_blocking_calls.blocking_calls_aggregation;
        SELECT
          bc.id,
          bc.call_id,
          bc.name,
          bc.ts,
          bc.dur,
          bc.cuj_name,
          bc.frame_id,
          thread.name AS thread_name
        FROM android_cuj_frame_blocking_calls AS bc
        JOIN thread USING (utid)
        ORDER BY bc.id;
        """,
        out=Csv("""
        "id","call_id","name","ts","dur","cuj_name","frame_id","thread_name"
        1,1,"DrawFrames",27000000,3000000,"COMPLETED_CUJ",20,"RenderThread"
        2,3,"hwuiTask",27200000,500000,"COMPLETED_CUJ",20,"hwuiTask0"
        3,3,"hwuiTask",27700000,800000,"COMPLETED_CUJ",20,"hwuiTask1"
        4,2,"animation",28000000,1000000,"COMPLETED_CUJ",20,"UiThread"
        """))
