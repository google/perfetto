#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint, TraceInjector
from python.generators.diff_tests.testing import TestSuite


class GraphicsParser(TestSuite):
  # Contains tests for graphics related events and tables. Graphics frame
  # trace tests.
  def test_graphics_frame_events(self):
    return DiffTestBlueprint(
        trace=Path('graphics_frame_events.py'),
        query="""
        SELECT ts, gpu_track.name AS track_name, dur, frame_slice.name AS slice_name,
          frame_number, layer_name
        FROM gpu_track
        LEFT JOIN frame_slice ON gpu_track.id = frame_slice.track_id
        WHERE scope = 'graphics_frame_event'
        ORDER BY ts;
        """,
        out=Csv('''
          "ts","track_name","dur","slice_name","frame_number","layer_name"
          1,"Buffer: 1 layer1",0,"Dequeue",11,"layer1"
          1,"APP_1 layer1",3,"11",11,"layer1"
          4,"Buffer: 1 layer1",0,"Queue",11,"layer1"
          4,"GPU_1 layer1",2,"11",11,"layer1"
          6,"Buffer: 1 layer1",0,"AcquireFenceSignaled",11,"layer1"
          6,"Buffer: 2 layer2",0,"Dequeue",12,"layer2"
          6,"APP_2 layer2",3,"12",12,"layer2"
          7,"Buffer: 7 layer7",0,"unknown_event",15,"layer7"
          8,"Buffer: 1 layer1",0,"Latch",11,"layer1"
          8,"Buffer: 2 layer2",0,"AcquireFenceSignaled",12,"layer2"
          8,"SF_1 layer1",6,"11",11,"layer1"
          9,"Buffer: 2 layer2",0,"Queue",12,"layer2"
          11,"Buffer: 2 layer2",0,"Latch",12,"layer2"
          11,"SF_2 layer2",5,"12",12,"layer2"
          14,"Buffer: 1 layer1",0,"PresentFenceSignaled",11,"layer1"
          14,"Display_layer1",10,"11",11,"layer1"
          16,"Buffer: 2 layer2",0,"PresentFenceSignaled",12,"layer2"
          16,"Display_layer2",-1,"12",12,"layer2"
          24,"Buffer: 1 layer1",0,"PresentFenceSignaled",13,"layer1"
          24,"Display_layer1",-1,"13",13,"layer1"
          31,"Buffer: 1 layer1",0,"Dequeue",21,"layer1"
          31,"APP_1 layer1",3,"21",21,"layer1"
          34,"Buffer: 1 layer1",0,"Queue",21,"layer1"
          34,"GPU_1 layer1",-1,"21",21,"layer1"
          37,"Buffer: 1 layer1",0,"Dequeue",22,"layer1"
          37,"APP_1 layer1",4,"22",22,"layer1"
          41,"Buffer: 1 layer1",0,"Queue",22,"layer1"
          41,"GPU_1 layer1",5,"22",22,"layer1"
          46,"Buffer: 1 layer1",0,"AcquireFenceSignaled",22,"layer1"
          53,"Buffer: 2 layer2",0,"Dequeue",24,"layer2"
          53,"APP_2 layer2",-1,"0",0,"layer2"
          59,"Buffer: 2 layer2",0,"AcquireFenceSignaled",24,"layer2"
          61,"Buffer: 2 layer2",0,"Latch",24,"layer2"
          61,"SF_2 layer2",-1,"24",24,"layer2"
          63,"Buffer: 1 layer1",0,"Dequeue",25,"layer1"
          63,"APP_1 layer1",-1,"0",0,"layer1"
          73,"Buffer: 1 layer1",0,"Dequeue",26,"layer1"
          73,"APP_1 layer1",2,"26",26,"layer1"
          75,"Buffer: 1 layer1",0,"Queue",26,"layer1"
          75,"GPU_1 layer1",4,"26",26,"layer1"
          79,"Buffer: 1 layer1",0,"AcquireFenceSignaled",26,"layer1"
          81,"Buffer: 1 layer1",0,"Dequeue",30,"layer1"
          81,"APP_1 layer1",2,"30",30,"layer1"
          83,"Buffer: 1 layer1",0,"Queue",30,"layer1"
          83,"GPU_1 layer1",-1,"30",30,"layer1"
          90,"Buffer: 1 layer2",0,"Dequeue",35,"layer2"
          90,"APP_1 layer2",2,"35",35,"layer2"
          92,"Buffer: 1 layer2",0,"Queue",35,"layer2"
          92,"GPU_1 layer2",-1,"35",35,"layer2"
        '''))

  # GPU Memory ftrace packets
  def test_gpu_mem_total(self):
    return DiffTestBlueprint(
        trace=Path('gpu_mem_total.py'),
        query='''
          SELECT ct.name, ct.unit, c.ts, p.pid, cast_int!(c.value) AS value
          FROM counter_track ct
          LEFT JOIN process_counter_track pct USING (id)
          LEFT JOIN process p USING (upid)
          LEFT JOIN counter c ON c.track_id = ct.id
          ORDER BY ts;
        ''',
        out=Csv("""
          "name","unit","ts","pid","value"
          "GPU Memory","bytes",0,"[NULL]",123
          "GPU Memory","bytes",0,1,100
          "GPU Memory","bytes",5,"[NULL]",256
          "GPU Memory","bytes",5,1,233
          "GPU Memory","bytes",10,"[NULL]",123
          "GPU Memory","bytes",10,1,0
        """))

  def test_gpu_mem_total_after_free_gpu_mem_total(self):
    return DiffTestBlueprint(
        trace=Path('gpu_mem_total_after_free.py'),
        query='''
          SELECT ct.name, ct.unit, c.ts, p.pid, cast_int!(c.value) AS value
          FROM counter_track ct
          LEFT JOIN process_counter_track pct USING (id)
          LEFT JOIN process p USING (upid)
          LEFT JOIN counter c ON c.track_id = ct.id
          ORDER BY ts;
        ''',
        out=Csv("""
          "name","unit","ts","pid","value"
          "GPU Memory","bytes",0,1,100
          "GPU Memory","bytes",5,1,233
          "GPU Memory","bytes",10,1,50
        """))

  # Clock sync
  def test_clock_sync(self):
    return DiffTestBlueprint(
        trace=Path('clock_sync.py'),
        query="""
        SELECT ts, cast(value AS integer) AS int_value
        FROM counters
        WHERE name GLOB 'gpu_counter*';
        """,
        out=Csv("""
        "ts","int_value"
        1,5
        102,7
        1003,9
        1005,11
        2006,12
        2010,13
        2013,14
        3007,15
        3010,0
        """))

  # Frame Timeline event trace tests
  def test_expected_frame_timeline_events(self):
    return DiffTestBlueprint(
        trace=Path('frame_timeline_events.py'),
        query=Path('expected_frame_timeline_events_test.sql'),
        out=Csv("""
        "ts","dur","pid","display_frame_token","surface_frame_token","layer_name"
        20,6,666,2,"[NULL]","[NULL]"
        21,15,1000,4,1,"Layer1"
        40,6,666,4,"[NULL]","[NULL]"
        41,15,1000,6,5,"Layer1"
        80,6,666,6,"[NULL]","[NULL]"
        90,16,1000,8,7,"Layer1"
        120,6,666,8,"[NULL]","[NULL]"
        140,6,666,12,"[NULL]","[NULL]"
        150,20,1000,15,14,"Layer1"
        170,6,666,15,"[NULL]","[NULL]"
        200,6,666,17,"[NULL]","[NULL]"
        220,-1,666,18,"[NULL]","[NULL]"
        220,10,666,18,"[NULL]","[NULL]"
        """))

  def test_actual_frame_timeline_events(self):
    return DiffTestBlueprint(
        trace=Path('frame_timeline_events.py'),
        query='''
          SELECT ts, dur, process.pid, display_frame_token, surface_frame_token, layer_name,
            present_type, on_time_finish, gpu_composition, jank_type, prediction_type, jank_tag, jank_severity_type
          FROM
            (SELECT t.*, process_track.name AS track_name FROM
              process_track LEFT JOIN actual_frame_timeline_slice t
              ON process_track.id = t.track_id) s
          JOIN process USING(upid)
          WHERE s.track_name = 'Actual Timeline'
          ORDER BY ts;
        ''',
        out=Csv("""
          "ts","dur","pid","display_frame_token","surface_frame_token","layer_name","present_type","on_time_finish","gpu_composition","jank_type","prediction_type","jank_tag","jank_severity_type"
          20,6,666,2,"[NULL]","[NULL]","On-time Present",1,0,"None","Valid Prediction","No Jank","None"
          21,16,1000,4,1,"Layer1","On-time Present",1,0,"None","Valid Prediction","No Jank","None"
          41,33,1000,6,5,"Layer1","Late Present",0,0,"App Deadline Missed","Valid Prediction","Self Jank","Full"
          42,5,666,4,"[NULL]","[NULL]","On-time Present",1,0,"None","Valid Prediction","No Jank","None"
          80,110,1000,17,16,"Layer1","Unknown Present",0,0,"Unknown Jank","Expired Prediction","Self Jank","Partial"
          81,7,666,6,"[NULL]","[NULL]","On-time Present",1,0,"None","Valid Prediction","No Jank","None"
          90,16,1000,8,7,"Layer1","Early Present",1,0,"SurfaceFlinger Scheduling","Valid Prediction","Other Jank","Unknown"
          108,4,666,8,"[NULL]","[NULL]","Early Present",1,0,"SurfaceFlinger Scheduling","Valid Prediction","Self Jank","Unknown"
          148,8,666,12,"[NULL]","[NULL]","Late Present",0,0,"SurfaceFlinger Scheduling, SurfaceFlinger CPU Deadline Missed","Valid Prediction","Self Jank","Unknown"
          150,17,1000,15,14,"Layer1","On-time Present",1,0,"None","Valid Prediction","No Jank","None"
          150,17,1000,15,14,"Layer2","On-time Present",1,0,"None","Valid Prediction","No Jank","None"
          170,6,666,15,"[NULL]","[NULL]","On-time Present",1,0,"None","Valid Prediction","No Jank","None"
          200,6,666,17,"[NULL]","[NULL]","On-time Present",1,0,"None","Valid Prediction","No Jank","None"
          245,-1,666,18,"[NULL]","[NULL]","Late Present",0,0,"SurfaceFlinger Stuffing","Valid Prediction","SurfaceFlinger Stuffing","Unknown"
          245,15,666,18,"[NULL]","[NULL]","Dropped Frame",0,0,"Dropped Frame","Unspecified Prediction","Dropped Frame","Unknown"
        """))

  # Video 4 Linux 2 related tests
  def test_v4l2_vidioc_slice(self):
    return DiffTestBlueprint(
        trace=Path('v4l2_vidioc.textproto'),
        query="""
        SELECT ts, dur, name
        FROM slice
        WHERE category = 'Video 4 Linux 2';
        """,
        out=Csv("""
        "ts","dur","name"
        593268475912,0,"VIDIOC_QBUF minor=0 seq=0 type=9 index=19"
        593268603800,0,"VIDIOC_QBUF minor=0 seq=0 type=9 index=20"
        593528238295,0,"VIDIOC_DQBUF minor=0 seq=0 type=9 index=19"
        593544028229,0,"VIDIOC_DQBUF minor=0 seq=0 type=9 index=20"
        """))

  def test_v4l2_vidioc_flow(self):
    return DiffTestBlueprint(
        trace=Path('v4l2_vidioc.textproto'),
        query="""
        SELECT qbuf.ts, qbuf.dur, qbuf.name, dqbuf.ts, dqbuf.dur, dqbuf.name
        FROM flow
        JOIN slice qbuf ON flow.slice_out = qbuf.id
        JOIN slice dqbuf ON flow.slice_in = dqbuf.id;
        """,
        out=Path('v4l2_vidioc_flow.out'))

  def test_virtio_video_slice(self):
    return DiffTestBlueprint(
        trace=Path('virtio_video.textproto'),
        query="""
        SELECT slice.ts, slice.dur, slice.name, track.name
        FROM slice
        JOIN track ON slice.track_id = track.id;
        """,
        out=Csv("""
        "ts","dur","name","name"
        593125003271,84500592,"Resource #102","virtio_video stream #4 OUTPUT"
        593125003785,100000,"RESOURCE_QUEUE","virtio_video stream #4 Requests"
        593125084611,709696,"Resource #62","virtio_video stream #3 OUTPUT"
        593125084935,100000,"RESOURCE_QUEUE","virtio_video stream #3 Requests"
        593125794194,100000,"RESOURCE_QUEUE","virtio_video stream #3 Responses"
        593209502603,100000,"RESOURCE_QUEUE","virtio_video stream #4 Responses"
        """))

  # virtgpu (drm/virtio) related tests
  def test_virtio_gpu(self):
    return DiffTestBlueprint(
        trace=Path('virtio_gpu.textproto'),
        query="""
        SELECT
          ts,
          dur,
          name
        FROM
          slice
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","name"
        1345090723759,1180312,"SUBMIT_3D"
        1345090746311,1167135,"CTX_DETACH_RESOURCE"
        """))

  # Tests gpu_track with machine_id ID.
  def test_graphics_frame_events_machine_id(self):
    return DiffTestBlueprint(
        trace=Path('graphics_frame_events.py'),
        trace_modifier=TraceInjector(['graphics_frame_event'],
                                     {'machine_id': 1001}),
        query="""
        SELECT ts, gpu_track.name AS track_name, dur, frame_slice.name AS slice_name,
          frame_number, layer_name
        FROM gpu_track
        LEFT JOIN frame_slice ON gpu_track.id = frame_slice.track_id
        WHERE scope = 'graphics_frame_event'
          AND gpu_track.machine_id IS NOT NULL
        ORDER BY ts;
        """,
        out=Csv('''
          "ts","track_name","dur","slice_name","frame_number","layer_name"
          1,"Buffer: 1 layer1",0,"Dequeue",11,"layer1"
          1,"APP_1 layer1",3,"11",11,"layer1"
          4,"Buffer: 1 layer1",0,"Queue",11,"layer1"
          4,"GPU_1 layer1",2,"11",11,"layer1"
          6,"Buffer: 1 layer1",0,"AcquireFenceSignaled",11,"layer1"
          6,"Buffer: 2 layer2",0,"Dequeue",12,"layer2"
          6,"APP_2 layer2",3,"12",12,"layer2"
          7,"Buffer: 7 layer7",0,"unknown_event",15,"layer7"
          8,"Buffer: 1 layer1",0,"Latch",11,"layer1"
          8,"Buffer: 2 layer2",0,"AcquireFenceSignaled",12,"layer2"
          8,"SF_1 layer1",6,"11",11,"layer1"
          9,"Buffer: 2 layer2",0,"Queue",12,"layer2"
          11,"Buffer: 2 layer2",0,"Latch",12,"layer2"
          11,"SF_2 layer2",5,"12",12,"layer2"
          14,"Buffer: 1 layer1",0,"PresentFenceSignaled",11,"layer1"
          14,"Display_layer1",10,"11",11,"layer1"
          16,"Buffer: 2 layer2",0,"PresentFenceSignaled",12,"layer2"
          16,"Display_layer2",-1,"12",12,"layer2"
          24,"Buffer: 1 layer1",0,"PresentFenceSignaled",13,"layer1"
          24,"Display_layer1",-1,"13",13,"layer1"
          31,"Buffer: 1 layer1",0,"Dequeue",21,"layer1"
          31,"APP_1 layer1",3,"21",21,"layer1"
          34,"Buffer: 1 layer1",0,"Queue",21,"layer1"
          34,"GPU_1 layer1",-1,"21",21,"layer1"
          37,"Buffer: 1 layer1",0,"Dequeue",22,"layer1"
          37,"APP_1 layer1",4,"22",22,"layer1"
          41,"Buffer: 1 layer1",0,"Queue",22,"layer1"
          41,"GPU_1 layer1",5,"22",22,"layer1"
          46,"Buffer: 1 layer1",0,"AcquireFenceSignaled",22,"layer1"
          53,"Buffer: 2 layer2",0,"Dequeue",24,"layer2"
          53,"APP_2 layer2",-1,"0",0,"layer2"
          59,"Buffer: 2 layer2",0,"AcquireFenceSignaled",24,"layer2"
          61,"Buffer: 2 layer2",0,"Latch",24,"layer2"
          61,"SF_2 layer2",-1,"24",24,"layer2"
          63,"Buffer: 1 layer1",0,"Dequeue",25,"layer1"
          63,"APP_1 layer1",-1,"0",0,"layer1"
          73,"Buffer: 1 layer1",0,"Dequeue",26,"layer1"
          73,"APP_1 layer1",2,"26",26,"layer1"
          75,"Buffer: 1 layer1",0,"Queue",26,"layer1"
          75,"GPU_1 layer1",4,"26",26,"layer1"
          79,"Buffer: 1 layer1",0,"AcquireFenceSignaled",26,"layer1"
          81,"Buffer: 1 layer1",0,"Dequeue",30,"layer1"
          81,"APP_1 layer1",2,"30",30,"layer1"
          83,"Buffer: 1 layer1",0,"Queue",30,"layer1"
          83,"GPU_1 layer1",-1,"30",30,"layer1"
          90,"Buffer: 1 layer2",0,"Dequeue",35,"layer2"
          90,"APP_1 layer2",2,"35",35,"layer2"
          92,"Buffer: 1 layer2",0,"Queue",35,"layer2"
          92,"GPU_1 layer2",-1,"35",35,"layer2"
        '''))
