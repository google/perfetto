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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Frames(TestSuite):

  def test_android_frames_get_frame_table_with_id(self):
    return DiffTestBlueprint(
        trace=Path('../../metrics/graphics/android_doframe_depth.py'),
        query="""
        INCLUDE PERFETTO MODULE android.frames.timeline;
        SELECT frame_id, name, depth
        FROM android_frames_choreographer_do_frame doframe
        JOIN slice USING(id);
        """,
        out=Csv("""
        "frame_id","name","depth"
        10,"Choreographer#doFrame 10",0
        11,"Choreographer#doFrame 11",1
        """))

  def test_android_frames_choreographer_do_frame(self):
    return DiffTestBlueprint(
        trace=Path('../../metrics/graphics/android_jank_cuj.py'),
        query="""
        INCLUDE PERFETTO MODULE android.frames.timeline;

        SELECT * FROM android_frames_choreographer_do_frame;
        """,
        out=Csv("""
        "id","frame_id","ui_thread_utid","upid","ts"
        2,10,2,2,0
        15,20,2,2,20000000
        22,30,2,2,30000000
        35,40,2,2,40000000
        46,60,2,2,70000000
        55,90,2,2,100000000
        63,100,2,2,200000000
        73,110,2,2,300000000
        79,120,2,2,400000000
        87,130,2,2,550000000
        93,140,2,2,608500000
        99,145,2,2,655000000
        102,150,2,2,700000000
        108,160,2,2,800000000
        140,1000,2,2,1100000000
        """))

  def test_android_frames_draw_frame(self):
    return DiffTestBlueprint(
        trace=Path('../../metrics/graphics/android_jank_cuj.py'),
        query="""
        INCLUDE PERFETTO MODULE android.frames.timeline;

        SELECT * FROM android_frames_draw_frame;
        """,
        out=Csv("""
        "id","frame_id","render_thread_utid","upid"
        8,10,4,2
        16,20,4,2
        23,30,4,2
        41,40,4,2
        50,60,4,2
        57,90,4,2
        60,90,4,2
        66,100,4,2
        69,100,4,2
        74,110,4,2
        80,120,4,2
        89,130,4,2
        95,140,4,2
        100,145,4,2
        105,150,4,2
        109,160,4,2
        146,1000,4,2
        """))

  def test_android_frames(self):
    return DiffTestBlueprint(
        trace=Path('../../metrics/graphics/android_jank_cuj.py'),
        query="""
        INCLUDE PERFETTO MODULE android.frames.timeline;

        SELECT * FROM android_frames;
        """,
        out=Csv("""
        "frame_id","ts","dur","do_frame_id","draw_frame_id","actual_frame_timeline_id","expected_frame_timeline_id","render_thread_utid","ui_thread_utid","actual_frame_timeline_count","expected_frame_timeline_count"
        10,0,16000000,2,8,1,0,4,2,1,1
        20,8000000,28000000,15,16,12,11,4,2,1,1
        30,30000000,25000000,22,23,21,20,4,2,1,1
        40,40000000,40000000,35,41,37,36,4,2,1,1
        60,70000000,20000000,46,50,48,47,4,2,2,1
        90,100000000,23000000,55,57,54,53,4,2,1,1
        90,100000000,23000000,55,60,54,53,4,2,1,1
        100,200000000,22000000,63,66,65,64,4,2,1,1
        100,200000000,22000000,63,69,65,64,4,2,1,1
        110,300000000,80000000,73,74,71,70,4,2,3,2
        120,400000000,61000000,79,80,78,77,4,2,2,2
        130,500000000,16000000,87,89,85,84,4,2,3,2
        140,608600000,17000000,93,95,94,91,4,2,2,2
        145,650000000,20000000,99,100,98,97,4,2,1,1
        150,700500000,14500000,102,105,104,103,4,2,1,1
        160,1070000000,16000000,108,109,132,107,4,2,1,2
        1000,1100000000,500000000,140,146,138,137,4,2,1,1
        """))

  def test_android_first_frame_after(self):
    return DiffTestBlueprint(
        trace=Path('../../metrics/graphics/android_jank_cuj.py'),
        query="""
        INCLUDE PERFETTO MODULE android.frames.timeline;

        SELECT frame_id FROM android_first_frame_after(100000000);
        """,
        out=Csv("""
        "frame_id"
        100
        """))

  def test_android_frames_overrun(self):
    return DiffTestBlueprint(
        trace=Path('../../metrics/graphics/android_jank_cuj.py'),
        query="""
        INCLUDE PERFETTO MODULE android.frames.per_frame_metrics;

        SELECT * FROM android_frames_overrun;
        """,
        out=Csv("""
        "frame_id","overrun"
        10,0
        20,8000000
        30,5000000
        40,20000000
        60,-10000000
        90,3000000
        100,2000000
        110,41000000
        120,41000000
        130,-18000000
        140,5600000
        145,0
        150,-5000000
        160,266000000
        190,0
        200,16000000
        1000,480000000
        """))

  def test_android_app_vsync_delay_per_frame(self):
    return DiffTestBlueprint(
        trace=Path('../../metrics/graphics/android_jank_cuj.py'),
        query="""
        INCLUDE PERFETTO MODULE android.frames.per_frame_metrics;

        SELECT * FROM android_app_vsync_delay_per_frame;
        """,
        out=Csv("""
        "frame_id","app_vsync_delay"
        10,0
        20,0
        30,0
        40,0
        60,0
        90,0
        100,0
        110,0
        120,0
        130,0
        140,8600000
        145,0
        150,500000
        160,270000000
        1000,0
        """))

  def test_android_cpu_time_per_frame(self):
    return DiffTestBlueprint(
        trace=Path('../../metrics/graphics/android_jank_cuj.py'),
        query="""
        INCLUDE PERFETTO MODULE android.frames.per_frame_metrics;

        SELECT * FROM android_cpu_time_per_frame;
        """,
        out=Csv("""
        "frame_id","app_vsync_delay","do_frame_dur","draw_frame_dur","cpu_time"
        10,0,5000000,1000000,6000000
        20,0,3000000,4000000,7000000
        30,0,3000000,19000000,22000000
        40,0,13000000,7000000,20000000
        60,0,10000000,9000000,19000000
        90,0,15000000,8000000,23000000
        100,0,15000000,8000000,23000000
        110,0,15000000,2000000,17000000
        120,0,15000000,2000000,17000000
        130,0,5000000,4000000,9000000
        140,8600000,1500000,17000000,27100000
        145,0,20000000,3000000,23000000
        150,500000,2000000,13800000,16300000
        160,270000000,2000000,1000000,273000000
        1000,0,100000000,150000000,250000000
        """))

  def test_android_frame_stats(self):
    return DiffTestBlueprint(
        trace=Path('../../metrics/graphics/android_jank_cuj.py'),
        query="""
        INCLUDE PERFETTO MODULE android.frames.per_frame_metrics;

        SELECT * FROM android_frame_stats;
        """,
        out=Csv("""
        "frame_id","overrun","cpu_time","ui_time","was_jank","was_slow_frame","was_big_jank","was_huge_jank"
        10,0,6000000,5000000,"[NULL]","[NULL]","[NULL]","[NULL]"
        20,8000000,7000000,3000000,1,"[NULL]","[NULL]","[NULL]"
        30,5000000,22000000,3000000,1,1,"[NULL]","[NULL]"
        40,20000000,20000000,13000000,1,"[NULL]","[NULL]","[NULL]"
        60,-10000000,19000000,10000000,"[NULL]","[NULL]","[NULL]","[NULL]"
        90,3000000,23000000,15000000,1,1,"[NULL]","[NULL]"
        100,2000000,23000000,15000000,1,1,"[NULL]","[NULL]"
        110,41000000,17000000,15000000,1,"[NULL]","[NULL]","[NULL]"
        120,41000000,17000000,15000000,1,"[NULL]","[NULL]","[NULL]"
        130,-18000000,9000000,5000000,"[NULL]","[NULL]","[NULL]","[NULL]"
        140,5600000,27100000,1500000,1,1,"[NULL]","[NULL]"
        145,0,23000000,20000000,"[NULL]",1,"[NULL]","[NULL]"
        150,-5000000,16300000,2000000,"[NULL]","[NULL]","[NULL]","[NULL]"
        160,266000000,273000000,2000000,1,1,1,1
        1000,480000000,250000000,100000000,1,1,1,1
        """))
