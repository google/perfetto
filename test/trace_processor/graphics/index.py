#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Graphics(DiffTestModule):

  def test_gpu_counters(self):
    return DiffTestBlueprint(
        trace=Path('gpu_counters.py'),
        query=Path('gpu_counters_test.sql'),
        out=Csv("""
"ts","value","name","gpu_id","description","unit"
11,5.000000,"Vertex / Second",0,"Number of vertices per second","25/22"
12,7.000000,"Fragment / Second",0,"Number of fragments per second","26/22"
14,0.000000,"Triangle Acceleration",0,"Number of triangles per ms-ms","27/21:21"
21,10.000000,"Vertex / Second",0,"Number of vertices per second","25/22"
22,14.000000,"Fragment / Second",0,"Number of fragments per second","26/22"
24,9.000000,"Triangle Acceleration",0,"Number of triangles per ms-ms","27/21:21"
31,15.000000,"Vertex / Second",0,"Number of vertices per second","25/22"
32,21.000000,"Fragment / Second",0,"Number of fragments per second","26/22"
34,7.000000,"Triangle Acceleration",0,"Number of triangles per ms-ms","27/21:21"
"""))

  def test_gpu_counter_specs(self):
    return DiffTestBlueprint(
        trace=Path('gpu_counter_specs.textproto'),
        query=Path('gpu_counter_specs_test.sql'),
        out=Csv("""
"group_id","name","description","unit"
0,"GPU Frequency","clock speed","/22"
3,"Fragments / vertex","Number of fragments per vertex","39/25"
2,"Fragments / vertex","Number of fragments per vertex","39/25"
3,"Fragment / Second","Number of fragments per second","26/22"
4,"Triangle Acceleration","Number of triangles per ms-ms","27/21:21"
"""))

  def test_gpu_render_stages(self):
    return DiffTestBlueprint(
        trace=Path('gpu_render_stages.py'),
        query=Path('gpu_render_stages_test.sql'),
        out=Path('gpu_render_stages.out'))

  def test_gpu_render_stages_interned_spec(self):
    return DiffTestBlueprint(
        trace=Path('gpu_render_stages_interned_spec.textproto'),
        query=Path('gpu_render_stages_test.sql'),
        out=Path('gpu_render_stages_interned_spec.out'))

  def test_vulkan_api_events(self):
    return DiffTestBlueprint(
        trace=Path('vulkan_api_events.py'),
        query=Path('vulkan_api_events_test.sql'),
        out=Path('vulkan_api_events.out'))

  def test_gpu_log(self):
    return DiffTestBlueprint(
        trace=Path('gpu_log.py'),
        query=Path('gpu_log_test.sql'),
        out=Csv("""
"scope","track_name","ts","dur","slice_name","key","value"
"gpu_log","GPU Log",1,0,"VERBOSE","message","message0"
"gpu_log","GPU Log",1,0,"VERBOSE","tag","tag0"
"gpu_log","GPU Log",2,0,"DEBUG","message","message1"
"gpu_log","GPU Log",2,0,"DEBUG","tag","tag0"
"gpu_log","GPU Log",3,0,"INFO","message","message2"
"gpu_log","GPU Log",3,0,"INFO","tag","tag0"
"gpu_log","GPU Log",4,0,"ERROR","message","message4"
"gpu_log","GPU Log",4,0,"ERROR","tag","tag0"
"gpu_log","GPU Log",4,0,"WARNING","message","message3"
"gpu_log","GPU Log",4,0,"WARNING","tag","tag0"
"gpu_log","GPU Log",5,0,"VERBOSE","message","message5"
"gpu_log","GPU Log",5,0,"VERBOSE","tag","tag1"
"""))

  def test_graphics_frame_events(self):
    return DiffTestBlueprint(
        trace=Path('graphics_frame_events.py'),
        query=Path('graphics_frame_events_test.sql'),
        out=Path('graphics_frame_events.out'))

  def test_gpu_mem_total(self):
    return DiffTestBlueprint(
        trace=Path('gpu_mem_total.py'),
        query=Path('gpu_mem_total_test.sql'),
        out=Csv("""
"name","unit","description","ts","pid","value"
"GPU Memory","7","Total GPU memory used by the entire system",0,"[NULL]",123
"GPU Memory","7","Total GPU memory used by this process",0,1,100
"GPU Memory","7","Total GPU memory used by the entire system",5,"[NULL]",256
"GPU Memory","7","Total GPU memory used by this process",5,1,233
"GPU Memory","7","Total GPU memory used by the entire system",10,"[NULL]",123
"GPU Memory","7","Total GPU memory used by this process",10,1,0
"""))

  def test_gpu_mem_total_after_free_gpu_mem_total(self):
    return DiffTestBlueprint(
        trace=Path('gpu_mem_total_after_free.py'),
        query=Path('gpu_mem_total_test.sql'),
        out=Csv("""
"name","unit","description","ts","pid","value"
"GPU Memory","7","Total GPU memory used by this process",0,1,100
"GPU Memory","7","Total GPU memory used by this process",5,1,233
"GPU Memory","7","Total GPU memory used by this process",10,1,50
"""))

  def test_clock_sync(self):
    return DiffTestBlueprint(
        trace=Path('clock_sync.py'),
        query=Path('clock_sync_test.sql'),
        out=Csv("""
"ts","int_value"
1,3
102,5
1003,7
1005,9
2006,11
2010,12
2013,13
3007,14
3010,15
"""))

  def test_frame_missed_event_frame_missed(self):
    return DiffTestBlueprint(
        trace=Path('frame_missed.py'),
        query=Path('frame_missed_event_test.sql'),
        out=Csv("""
"ts","dur"
100,1
102,1
103,1
"""))

  def test_frame_missed_metrics(self):
    return DiffTestBlueprint(
        trace=Path('frame_missed.py'),
        query=Metric('android_surfaceflinger'),
        out=TextProto(r"""
android_surfaceflinger {
  missed_frames: 3
  missed_hwc_frames: 0
  missed_gpu_frames: 0
  missed_frame_rate: 0.42857142857142855 # = 3/7
  gpu_invocations: 0
}
"""))

  def test_surfaceflinger_gpu_invocation(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_gpu_invocation.py'),
        query=Metric('android_surfaceflinger'),
        out=TextProto(r"""
android_surfaceflinger {
  missed_frames: 0
  missed_hwc_frames: 0
  missed_gpu_frames: 0
  gpu_invocations: 4
  avg_gpu_waiting_dur_ms: 4
  total_non_empty_gpu_waiting_dur_ms: 11
}
"""))

  def test_gpu_metric(self):
    return DiffTestBlueprint(
        trace=Path('gpu_metric.py'),
        query=Metric('android_gpu'),
        out=Path('gpu_metric.out'))

  def test_gpu_frequency_metric(self):
    return DiffTestBlueprint(
        trace=Path('gpu_frequency_metric.textproto'),
        query=Metric('android_gpu'),
        out=Path('gpu_frequency_metric.out'))

  def test_android_jank_cuj(self):
    return DiffTestBlueprint(
        trace=Path('android_jank_cuj.py'),
        query=Metric('android_jank_cuj'),
        out=Path('android_jank_cuj.out'))

  def test_android_jank_cuj_query(self):
    return DiffTestBlueprint(
        trace=Path('android_jank_cuj.py'),
        query=Path('android_jank_cuj_query_test.sql'),
        out=Path('android_jank_cuj_query.out'))

  def test_expected_frame_timeline_events(self):
    return DiffTestBlueprint(
        trace=Path('frame_timeline_events.py'),
        query=Path('expected_frame_timeline_events_test.sql'),
        out=Csv("""
"ts","dur","pid","display_frame_token","surface_frame_token","layer_name"
20,6,666,2,0,"[NULL]"
21,15,1000,4,1,"Layer1"
40,6,666,4,0,"[NULL]"
41,15,1000,6,5,"Layer1"
80,6,666,6,0,"[NULL]"
90,16,1000,8,7,"Layer1"
120,6,666,8,0,"[NULL]"
140,6,666,12,0,"[NULL]"
150,20,1000,15,14,"Layer1"
170,6,666,15,0,"[NULL]"
200,6,666,17,0,"[NULL]"
220,10,666,18,0,"[NULL]"
"""))

  def test_actual_frame_timeline_events(self):
    return DiffTestBlueprint(
        trace=Path('frame_timeline_events.py'),
        query=Path('actual_frame_timeline_events_test.sql'),
        out=Path('actual_frame_timeline_events.out'))

  def test_composition_layer_count(self):
    return DiffTestBlueprint(
        trace=Path('composition_layer.py'),
        query=Path('composition_layer_count_test.sql'),
        out=Csv("""
"AVG(value)"
3.000000
"""))

  def test_g2d_metrics(self):
    return DiffTestBlueprint(
        trace=Path('g2d_metrics.textproto'),
        query=Metric('g2d'),
        out=Path('g2d_metrics.out'))

  def test_composer_execution(self):
    return DiffTestBlueprint(
        trace=Path('composer_execution.py'),
        query=Path('composer_execution_test.sql'),
        out=Csv("""
"validation_type","count","total"
"separated_validation",1,200
"skipped_validation",2,200
"unskipped_validation",1,200
"""))

  def test_display_metrics(self):
    return DiffTestBlueprint(
        trace=Path('display_metrics.py'),
        query=Metric('display_metrics'),
        out=Path('display_metrics.out'))

  def test_dpu_vote_clock_bw(self):
    return DiffTestBlueprint(
        trace=Path('dpu_vote_clock_bw.textproto'),
        query=Metric('android_hwcomposer'),
        out=Path('dpu_vote_clock_bw.out'))

  def test_drm_vblank_gpu_track(self):
    return DiffTestBlueprint(
        trace=Path('drm_vblank.textproto'),
        query=Path('drm_gpu_track_test.sql'),
        out=Csv("""
"name","ts","dur","name","flat_key","int_value","string_value"
"vblank-0",6159770881976,0,"signal","vblank seqno",3551,"[NULL]"
"vblank-0",6159770993376,0,"deliver","vblank seqno",3551,"[NULL]"
"""))

  def test_drm_sched_gpu_track(self):
    return DiffTestBlueprint(
        trace=Path('drm_sched.textproto'),
        query=Path('drm_gpu_track_test.sql'),
        out=Csv("""
"name","ts","dur","name","flat_key","int_value","string_value"
"sched-ring0",9246165349383,4729073,"job","gpu sched job",13481,"[NULL]"
"sched-ring0",9246170078456,3941571,"job","gpu sched job",13482,"[NULL]"
"sched-ring0",9246174020027,25156,"job","gpu sched job",13483,"[NULL]"
"sched-ring0",9246181933273,4726312,"job","gpu sched job",13484,"[NULL]"
"""))

  def test_drm_sched_thread_track(self):
    return DiffTestBlueprint(
        trace=Path('drm_sched.textproto'),
        query=Path('drm_thread_track_test.sql'),
        out=Csv("""
"utid","ts","dur","name","flat_key","int_value","string_value"
1,9246165326050,0,"drm_sched_job","gpu sched ring","[NULL]","ring0"
1,9246165326050,0,"drm_sched_job","gpu sched job",13481,"[NULL]"
3,9246166957616,0,"drm_sched_job","gpu sched ring","[NULL]","ring0"
3,9246166957616,0,"drm_sched_job","gpu sched job",13482,"[NULL]"
3,9246167272512,0,"drm_sched_job","gpu sched ring","[NULL]","ring0"
3,9246167272512,0,"drm_sched_job","gpu sched job",13483,"[NULL]"
1,9246181907439,0,"drm_sched_job","gpu sched ring","[NULL]","ring0"
1,9246181907439,0,"drm_sched_job","gpu sched job",13484,"[NULL]"
"""))

  def test_drm_dma_fence_gpu_track(self):
    return DiffTestBlueprint(
        trace=Path('drm_dma_fence.textproto'),
        query=Path('drm_gpu_track_test.sql'),
        out=Csv("""
"name","ts","dur","name","flat_key","int_value","string_value"
"fence-gpu-ring-0-1",11303602488073,12813,"fence","fence seqno",16665,"[NULL]"
"fence-gpu-ring-0-1",11303602500886,4805626,"fence","fence seqno",16665,"[NULL]"
"fence-gpu-ring-0-1",11303607306512,3850783,"fence","fence seqno",16666,"[NULL]"
"fence-ring0-9",11303702681699,4868387,"fence","fence seqno",5065,"[NULL]"
"""))

  def test_drm_dma_fence_thread_track(self):
    return DiffTestBlueprint(
        trace=Path('drm_dma_fence.textproto'),
        query=Path('drm_thread_track_test.sql'),
        out=Csv("""
"utid","ts","dur","name","flat_key","int_value","string_value"
3,11303702851231,4867658,"dma_fence_wait","fence context",9,"[NULL]"
3,11303702851231,4867658,"dma_fence_wait","fence seqno",5065,"[NULL]"
"""))

  def test_v4l2_vidioc_slice(self):
    return DiffTestBlueprint(
        trace=Path('v4l2_vidioc.textproto'),
        query=Path('v4l2_vidioc_slice_test.sql'),
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
        query=Path('v4l2_vidioc_flow_test.sql'),
        out=Path('v4l2_vidioc_flow.out'))

  def test_virtio_video_slice(self):
    return DiffTestBlueprint(
        trace=Path('virtio_video.textproto'),
        query=Path('virtio_video_slice_test.sql'),
        out=Csv("""
"ts","dur","name","name"
593125003271,84500592,"Resource #102","virtio_video stream #4 OUTPUT"
593125003785,100000,"RESOURCE_QUEUE","virtio_video stream #4 Requests"
593125084611,709696,"Resource #62","virtio_video stream #3 OUTPUT"
593125084935,100000,"RESOURCE_QUEUE","virtio_video stream #3 Requests"
593125794194,100000,"RESOURCE_QUEUE","virtio_video stream #3 Responses"
593209502603,100000,"RESOURCE_QUEUE","virtio_video stream #4 Responses"
"""))

  def test_virtio_gpu_test(self):
    return DiffTestBlueprint(
        trace=Path('virtio_gpu.textproto'),
        query=Path('virtio_gpu_test.sql'),
        out=Csv("""
"ts","dur","name"
1345090723759,1180312,"SUBMIT_3D"
1345090746311,1167135,"CTX_DETACH_RESOURCE"
"""))

  def test_mali_test(self):
    return DiffTestBlueprint(
        trace=Path('mali.textproto'),
        query=Path('mali_test.sql'),
        out=Csv("""
"ts","dur","name"
751796307210,4313965,"mali_KCPU_CQS_WAIT"
751800638997,0,"mali_KCPU_CQS_SET"
"""))

  def test_mali_fence_test(self):
    return DiffTestBlueprint(
        trace=Path('mali_fence.textproto'),
        query=Path('mali_fence_test.sql'),
        out=Csv("""
"ts","dur","name"
751796307210,4313965,"mali_KCPU_FENCE_WAIT"
751800638997,0,"mali_KCPU_FENCE_SIGNAL"
"""))
