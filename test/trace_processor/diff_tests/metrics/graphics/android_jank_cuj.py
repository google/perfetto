#!/usr/bin/env python3
# Copyright (C) 2020 The Android Open Source Project
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

from os import sys

import synth_common

# com.android.systemui
PID = 1000
# RenderThread
RTID = 1555
# Jit thread pool
JITID = 1777
LAYER = "TX - NotificationShade#0"

# /system/bin/surfaceflinger
SF_PID = 1050
# RenderEngine thread
SF_RETID = 1055

PROCESS_TRACK = 1234
FIRST_CUJ_TRACK = 321
SHADE_CUJ_TRACK = 654
CANCELED_CUJ_TRACK = 987


def add_instant_for_track(trace, ts, track, name):
  trace.add_track_event_slice(ts=ts, dur=0, track=track, name=name)


def add_main_thread_atrace(trace, ts, ts_end, buf):
  trace.add_atrace_begin(ts=ts, tid=PID, pid=PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=PID, pid=PID)


def add_render_thread_atrace_begin(trace, ts, buf):
  trace.add_atrace_begin(ts=ts, tid=RTID, pid=PID, buf=buf)


def add_render_thread_atrace_end(trace, ts_end):
  trace.add_atrace_end(ts=ts_end, tid=RTID, pid=PID)


def add_render_thread_atrace(trace, ts, ts_end, buf):
  add_render_thread_atrace_begin(trace, ts, buf)
  add_render_thread_atrace_end(trace, ts_end)


def add_gpu_thread_atrace(trace, ts, ts_end, buf):
  trace.add_atrace_begin(ts=ts, tid=1666, pid=PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=1666, pid=PID)


def add_jit_thread_atrace(trace, ts, ts_end, buf):
  trace.add_atrace_begin(ts=ts, tid=JITID, pid=PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=JITID, pid=PID)


def add_sf_main_thread_atrace(trace, ts, ts_end, buf):
  add_sf_main_thread_atrace_begin(trace, ts, buf)
  add_sf_main_thread_atrace_end(trace, ts_end)


def add_sf_main_thread_atrace_begin(trace, ts, buf):
  trace.add_atrace_begin(ts=ts, tid=SF_PID, pid=SF_PID, buf=buf)


def add_sf_main_thread_atrace_end(trace, ts_end):
  trace.add_atrace_end(ts=ts_end, tid=SF_PID, pid=SF_PID)


def add_sf_render_engine_atrace(trace, ts, ts_end, buf):
  trace.add_atrace_begin(ts=ts, tid=SF_RETID, pid=SF_PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=SF_RETID, pid=SF_PID)


def add_frame(trace,
              vsync,
              ts_do_frame,
              ts_end_do_frame,
              ts_draw_frame,
              ts_end_draw_frame,
              ts_gpu=None,
              ts_end_gpu=None,
              resync=False):
  add_main_thread_atrace(trace, ts_do_frame, ts_end_do_frame,
                         "Choreographer#doFrame %d" % vsync)
  if resync:
    add_main_thread_atrace(
        trace, ts_do_frame, ts_end_do_frame,
        "Choreographer#doFrame - resynced to %d in 0.0s" % (vsync + 1))
  gpu_idx = 1000 + vsync * 10 + 1
  if ts_gpu is None:
    gpu_fence_message = "GPU completion fence %d has signaled"
  else:
    gpu_fence_message = "Trace GPU completion fence %d"
  add_render_thread_atrace_begin(trace, ts_draw_frame, "DrawFrames %d" % vsync)
  add_render_thread_atrace(trace, ts_end_draw_frame - 100,
                           ts_end_draw_frame - 1, gpu_fence_message % gpu_idx)
  add_render_thread_atrace_end(trace, ts_end_draw_frame)

  if ts_gpu is not None:
    add_gpu_thread_atrace(trace, ts_gpu, ts_end_gpu,
                          "waiting for GPU completion %d" % gpu_idx)


def add_sf_frame(trace,
                 vsync,
                 ts_commit,
                 ts_end_commit,
                 ts_composite,
                 ts_end_composite,
                 ts_compose_surfaces=None,
                 ts_end_compose_surfaces=None):
  add_sf_main_thread_atrace(trace, ts_commit, ts_end_commit,
                            "commit %d" % vsync)
  add_sf_main_thread_atrace_begin(trace, ts_composite, "composite %d" % vsync)
  if ts_compose_surfaces is not None:
    add_sf_main_thread_atrace(trace, ts_compose_surfaces,
                              ts_end_compose_surfaces, "composeSurfaces")
  add_sf_main_thread_atrace_end(trace, ts_end_composite)


def add_expected_display_frame_events(ts, dur, token):
  trace.add_expected_display_frame_start_event(
      ts=ts, cookie=token, token=100 + token, pid=SF_PID)
  trace.add_frame_end_event(ts=ts + dur, cookie=token)


def add_expected_surface_frame_events(ts, dur, token):
  trace.add_expected_surface_frame_start_event(
      ts=ts,
      cookie=100000 + token,
      token=token,
      display_frame_token=100 + token,
      pid=PID,
      layer_name='')
  trace.add_frame_end_event(ts=ts + dur, cookie=100000 + token)


def add_actual_display_frame_events(ts, dur, token, cookie=None, jank=None):
  jank_type = jank if jank is not None else 1
  present_type = 2 if jank is not None else 1
  on_time_finish = 0 if jank is not None else 1
  trace.add_actual_display_frame_start_event(
      ts=ts,
      cookie=token + 1,
      token=100 + token,
      pid=SF_PID,
      present_type=present_type,
      on_time_finish=on_time_finish,
      gpu_composition=0,
      jank_type=jank_type,
      prediction_type=3)
  trace.add_frame_end_event(ts=ts + dur, cookie=token + 1)


def add_actual_surface_frame_events(ts,
                                    dur,
                                    token,
                                    cookie=None,
                                    jank=None,
                                    on_time_finish_override=None,
                                    display_frame_token_override=None,
                                    layer_name=LAYER):
  if cookie is None:
    cookie = token + 1
  jank_type = jank if jank is not None else 1
  present_type = 2 if jank is not None else 1
  if on_time_finish_override is None:
    on_time_finish = 1 if jank is None else 0
  else:
    on_time_finish = on_time_finish_override
  display_frame_token = display_frame_token_override or (token + 100)
  trace.add_actual_surface_frame_start_event(
      ts=ts,
      cookie=100002 + cookie,
      token=token,
      display_frame_token=display_frame_token,
      pid=PID,
      present_type=present_type,
      on_time_finish=on_time_finish,
      gpu_composition=0,
      jank_type=jank_type,
      prediction_type=3,
      layer_name=layer_name)
  trace.add_frame_end_event(ts=ts + dur, cookie=100002 + cookie)


trace = synth_common.create_trace()

trace.add_packet()
trace.add_package_list(
    ts=0, name="com.android.systemui", uid=10001, version_code=1)
trace.add_package_list(ts=0, name="android", uid=1000, version_code=1)
trace.add_process(pid=PID, ppid=1, cmdline="com.android.systemui", uid=10001)
trace.add_process(
    pid=SF_PID, ppid=1, cmdline="/system/bin/surfaceflinger", uid=1000)
trace.add_thread(
    tid=RTID, tgid=PID, cmdline="RenderThread", name="RenderThread")
trace.add_thread(
    tid=1666, tgid=PID, cmdline="GPU completion", name="GPU completion")
trace.add_thread(
    tid=JITID, tgid=PID, cmdline="Jit thread pool", name="Jit thread pool")
trace.add_thread(
    tid=SF_RETID, tgid=SF_PID, cmdline="RenderEngine", name="RenderEngine")
trace.add_process_track_descriptor(PROCESS_TRACK, pid=PID)
trace.add_track_descriptor(FIRST_CUJ_TRACK, parent=PROCESS_TRACK)
trace.add_track_descriptor(SHADE_CUJ_TRACK, parent=PROCESS_TRACK)
trace.add_track_descriptor(CANCELED_CUJ_TRACK, parent=PROCESS_TRACK)
trace.add_track_event_slice_begin(
    ts=5, track=FIRST_CUJ_TRACK, name="J<FIRST_CUJ>")
trace.add_track_event_slice_end(ts=100_000_000, track=FIRST_CUJ_TRACK)
trace.add_track_event_slice_begin(
    ts=10, track=SHADE_CUJ_TRACK, name="J<SHADE_ROW_EXPAND>")
trace.add_track_event_slice_end(ts=901_000_010, track=SHADE_CUJ_TRACK)
add_instant_for_track(trace, ts=11, track=SHADE_CUJ_TRACK, name="FT#layerId#0")
add_instant_for_track(
    trace,
    ts=950_100_000,
    track=SHADE_CUJ_TRACK,
    name="FT#MissedHWUICallback#150")
add_instant_for_track(
    trace,
    ts=950_100_000,
    track=SHADE_CUJ_TRACK,
    name="FT#MissedSFCallback#150")

trace.add_track_event_slice_begin(
    ts=100_100_000, track=CANCELED_CUJ_TRACK, name="J<CANCELED>")
trace.add_track_event_slice_end(ts=999_000_000, track=CANCELED_CUJ_TRACK)
trace.add_ftrace_packet(cpu=0)

trace.add_atrace_counter(
    ts=150_000_000, tid=PID, pid=PID, buf="J<FIRST_CUJ>#totalFrames", cnt=6)
trace.add_atrace_counter(
    ts=150_100_000, tid=PID, pid=PID, buf="J<FIRST_CUJ>#missedFrames", cnt=5)
trace.add_atrace_counter(
    ts=150_200_000, tid=PID, pid=PID, buf="J<FIRST_CUJ>#missedAppFrames", cnt=5)
trace.add_atrace_counter(
    ts=150_300_000, tid=PID, pid=PID, buf="J<FIRST_CUJ>#missedSfFrames", cnt=1)
trace.add_atrace_counter(
    ts=150_400_000,
    tid=PID,
    pid=PID,
    buf="J<FIRST_CUJ>#maxFrameTimeMillis",
    cnt=40)

trace.add_atrace_counter(
    ts=950_000_000,
    tid=PID,
    pid=PID,
    buf="J<SHADE_ROW_EXPAND>#totalFrames",
    cnt=13)
trace.add_atrace_counter(
    ts=950_100_000,
    tid=PID,
    pid=PID,
    buf="J<SHADE_ROW_EXPAND>#missedFrames",
    cnt=8)
trace.add_atrace_counter(
    ts=950_200_000,
    tid=PID,
    pid=PID,
    buf="J<SHADE_ROW_EXPAND>#missedAppFrames",
    cnt=7)
trace.add_atrace_counter(
    ts=950_300_000,
    tid=PID,
    pid=PID,
    buf="J<SHADE_ROW_EXPAND>#missedSfFrames",
    cnt=2)
trace.add_atrace_counter(
    ts=950_300_000,
    tid=PID,
    pid=PID,
    buf="J<SHADE_ROW_EXPAND>#maxSuccessiveMissedFrames",
    cnt=5)
trace.add_atrace_counter(
    ts=950_400_000,
    tid=PID,
    pid=PID,
    buf="J<SHADE_ROW_EXPAND>#maxFrameTimeMillis",
    cnt=62)

add_frame(
    trace,
    vsync=10,
    ts_do_frame=0,
    ts_end_do_frame=5_000_000,
    ts_draw_frame=4_000_000,
    ts_end_draw_frame=5_000_000,
    ts_gpu=10_000_000,
    ts_end_gpu=15_000_000,
    resync=True)
add_main_thread_atrace(
    trace, ts=1_500_000, ts_end=2_000_000, buf="binder transaction")
add_render_thread_atrace(
    trace, ts=4_500_000, ts_end=4_800_000, buf="flush layers")

add_sf_frame(
    trace,
    vsync=110,
    ts_commit=1_006_000_500,
    ts_end_commit=1_006_500_000,
    ts_composite=1_007_000_000,
    ts_end_composite=1_008_000_000)

# main thread binder call that delays the start of work on this frame
add_sf_main_thread_atrace(
    trace, ts=900_000_000, ts_end=1_006_000_000, buf="sf binder")

add_frame(
    trace,
    vsync=20,
    ts_do_frame=20_000_000,
    ts_end_do_frame=23_000_000,
    ts_draw_frame=22_000_000,
    ts_end_draw_frame=26_000_000,
    ts_gpu=27_500_000,
    ts_end_gpu=35_000_000)
add_main_thread_atrace(
    trace, ts=9_000_000, ts_end=19_000_000, buf="binder transaction")
add_render_thread_atrace(
    trace, ts=24_000_000, ts_end=25_000_000, buf="flush layers")

add_sf_frame(
    trace,
    vsync=120,
    ts_commit=1_016_000_000,
    ts_end_commit=1_018_000_000,
    ts_composite=1_020_500_000,
    ts_end_composite=1_025_000_000)

add_frame(
    trace,
    vsync=30,
    ts_do_frame=30_000_000,
    ts_end_do_frame=33_000_000,
    ts_draw_frame=31_000_000,
    ts_end_draw_frame=50_000_000,
    ts_gpu=51_500_000,
    ts_end_gpu=52_000_000)
add_main_thread_atrace(
    trace, ts=31_000_000, ts_end=31_050_000, buf="binder transaction")
add_main_thread_atrace(
    trace, ts=31_100_000, ts_end=31_150_000, buf="binder transaction")
add_main_thread_atrace(
    trace, ts=31_200_000, ts_end=31_250_000, buf="binder transaction")
add_main_thread_atrace(
    trace, ts=31_300_000, ts_end=31_350_000, buf="binder transaction")
add_main_thread_atrace(
    trace, ts=31_400_000, ts_end=31_450_000, buf="binder transaction")
add_main_thread_atrace(
    trace, ts=31_500_000, ts_end=31_550_000, buf="binder transaction")
add_main_thread_atrace(
    trace, ts=31_600_000, ts_end=31_650_000, buf="binder transaction")
add_main_thread_atrace(
    trace, ts=31_700_000, ts_end=31_750_000, buf="binder transaction")
add_main_thread_atrace(
    trace, ts=31_800_000, ts_end=31_850_000, buf="binder transaction")
add_render_thread_atrace(
    trace, ts=38_000_000, ts_end=50_000_000, buf="flush layers")

add_sf_frame(
    trace,
    vsync=130,
    ts_commit=1_032_000_000,
    ts_end_commit=1_033_000_000,
    ts_composite=1_034_000_000,
    ts_end_composite=1_045_000_000)

add_frame(
    trace,
    vsync=40,
    ts_do_frame=40_000_000,
    ts_end_do_frame=53_000_000,
    ts_draw_frame=52_000_000,
    ts_end_draw_frame=59_000_000,
    ts_gpu=66_500_000,
    ts_end_gpu=78_000_000)

add_jit_thread_atrace(
    trace,
    ts=39_000_000,
    ts_end=45_000_000,
    buf="JIT compiling void aa.aa(java.lang.Object, bb) (kind=Baseline)")
add_jit_thread_atrace(
    trace,
    ts=46_000_000,
    ts_end=47_000_000,
    buf="Lock contention on Jit code cache (owner tid: 12345)")
add_jit_thread_atrace(
    trace,
    ts=52_500_000,
    ts_end=54_000_000,
    buf="JIT compiling void cc.bb(java.lang.Object, bb) (kind=Osr)")
add_jit_thread_atrace(
    trace,
    ts=56_500_000,
    ts_end=60_000_000,
    buf="JIT compiling void ff.zz(java.lang.Object, bb) (kind=Baseline)")

# Main thread Running for 14 millis
trace.add_sched(ts=39_000_000, prev_pid=0, next_pid=PID)
trace.add_sched(ts=53_000_000, prev_pid=PID, next_pid=0, prev_state='R')

# RenderThread Running for 5 millis
trace.add_sched(ts=54_000_000, prev_pid=0, next_pid=RTID)
trace.add_sched(ts=59_000_000, prev_pid=RTID, next_pid=0, prev_state='R')

add_sf_frame(
    trace,
    vsync=140,
    ts_commit=1_048_000_000,
    ts_end_commit=1_049_000_000,
    ts_composite=1_055_000_000,
    ts_end_composite=1_060_000_000)

add_frame(
    trace,
    vsync=60,
    ts_do_frame=70_000_000,
    ts_end_do_frame=80_000_000,
    ts_draw_frame=78_000_000,
    ts_end_draw_frame=87_000_000,
    ts_gpu=86_500_000,
    ts_end_gpu=88_000_000)

# Main thread Running for 1 millis
trace.add_sched(ts=70_000_000, prev_pid=0, next_pid=PID)
trace.add_sched(ts=71_000_000, prev_pid=PID, next_pid=0, prev_state='R')

# RenderThread Running for 1 millis and R for 9.5 millis
trace.add_sched(ts=78_000_000, prev_pid=0, next_pid=RTID)
trace.add_sched(ts=78_500_000, prev_pid=RTID, next_pid=0, prev_state='R')
trace.add_sched(ts=78_500_000, prev_pid=0, next_pid=0)
trace.add_sched(ts=88_000_000, prev_pid=0, next_pid=RTID)
trace.add_sched(ts=88_500_000, prev_pid=RTID, next_pid=0, prev_state='R')

add_sf_frame(
    trace,
    vsync=160,
    ts_commit=1_070_000_000,
    ts_end_commit=1_071_000_000,
    ts_composite=1_072_000_000,
    ts_end_composite=1_080_000_000)

add_frame(
    trace,
    vsync=90,
    ts_do_frame=100_000_000,
    ts_end_do_frame=115_000_000,
    ts_draw_frame=102_000_000,
    ts_end_draw_frame=104_000_000,
    ts_gpu=108_000_000,
    ts_end_gpu=115_600_000)

add_render_thread_atrace_begin(trace, ts=108_000_000, buf="DrawFrames 90")
add_render_thread_atrace(
    trace,
    ts=113_000_000,
    ts_end=113_500_000,
    buf="Trace GPU completion fence 1902")
add_render_thread_atrace_end(trace, ts_end=114_000_000)

add_gpu_thread_atrace(
    trace,
    ts=121_500_000,
    ts_end=122_000_000,
    buf="waiting for GPU completion 1902")

add_sf_frame(
    trace,
    vsync=190,
    ts_commit=1_100_000_000,
    ts_end_commit=1_101_000_000,
    ts_composite=1_102_000_000,
    ts_end_composite=1_110_000_000,
    ts_compose_surfaces=1_104_000_000,
    ts_end_compose_surfaces=1_108_000_000)

add_sf_render_engine_atrace(
    trace, ts=1_104_500_000, ts_end=1_107_500_000, buf="REThreaded::drawLayers")
add_sf_render_engine_atrace(
    trace, ts=1_105_000_000, ts_end=1_107_000_000, buf="shader compile")

add_frame(
    trace,
    vsync=100,
    ts_do_frame=200_000_000,
    ts_end_do_frame=215_000_000,
    ts_draw_frame=202_000_000,
    ts_end_draw_frame=204_000_000,
    ts_gpu=208_000_000,
    ts_end_gpu=210_000_000)

add_render_thread_atrace(
    trace, ts=208_000_000, ts_end=214_000_000, buf="DrawFrames 100")

add_sf_frame(
    trace,
    vsync=200,
    ts_commit=1_200_000_000,
    ts_end_commit=1_202_000_000,
    ts_composite=1_203_000_000,
    ts_end_composite=1_232_000_000,
    ts_compose_surfaces=1_205_000_000,
    ts_end_compose_surfaces=1_230_000_000)

# shader compile from outside the frame that delays REThreaded::drawLayers
add_sf_render_engine_atrace(
    trace, ts=1_150_000_000, ts_end=1_207_000_000, buf="shader compile")

add_sf_render_engine_atrace(
    trace, ts=1_208_000_000, ts_end=1_229_000_000, buf="REThreaded::drawLayers")
add_sf_render_engine_atrace(
    trace, ts=1_209_000_000, ts_end=1_228_000_000, buf="shader compile")

add_frame(
    trace,
    vsync=110,
    ts_do_frame=300_000_000,
    ts_end_do_frame=315_000_000,
    ts_draw_frame=302_000_000,
    ts_end_draw_frame=304_000_000,
    ts_gpu=None,
    ts_end_gpu=None)

add_render_thread_atrace(
    trace, ts=305_000_000, ts_end=308_000_000, buf="dispatchFrameCallbacks")

add_frame(
    trace,
    vsync=120,
    ts_do_frame=400_000_000,
    ts_end_do_frame=415_000_000,
    ts_draw_frame=402_000_000,
    ts_end_draw_frame=404_000_000,
    ts_gpu=408_000_000,
    ts_end_gpu=410_000_000)

add_render_thread_atrace(
    trace, ts=415_000_000, ts_end=418_000_000, buf="dispatchFrameCallbacks")

# Frame start delayed by 50ms by a long binder transaction
add_main_thread_atrace(
    trace, ts=500_000_000, ts_end=549_500_000, buf="binder transaction")

add_frame(
    trace,
    vsync=130,
    ts_do_frame=550_000_000,
    ts_end_do_frame=555_000_000,
    ts_draw_frame=552_000_000,
    ts_end_draw_frame=556_000_000,
    ts_gpu=None,
    ts_end_gpu=None)

# Frame start delayed by 8ms by a long binder transaction
add_main_thread_atrace(
    trace, ts=600_000_000, ts_end=608_049_000, buf="binder transaction")

add_frame(
    trace,
    vsync=140,
    ts_do_frame=608_500_000,
    ts_end_do_frame=610_000_000,
    ts_draw_frame=609_000_000,
    ts_end_draw_frame=626_000_000,
    ts_gpu=None,
    ts_end_gpu=None)

add_frame(
    trace,
    vsync=145,
    ts_do_frame=655_000_000,
    ts_end_do_frame=675_000_000,
    ts_draw_frame=657_000_000,
    ts_end_draw_frame=660_000_000,
    ts_gpu=None,
    ts_end_gpu=None)

# Actual timeline slice starts 0.5ms after doFrame
add_frame(
    trace,
    vsync=150,
    ts_do_frame=700_000_000,
    ts_end_do_frame=702_000_000,
    ts_draw_frame=701_200_000,
    ts_end_draw_frame=715_000_000,
    ts_gpu=None,
    ts_end_gpu=None)

# Frame without a matching actual timeline slice
# Skipped in `android_jank_cuj.sql` since we assume the process
# did not draw anything.
add_frame(
    trace,
    vsync=160,
    ts_do_frame=800_000_000,
    ts_end_do_frame=802_000_000,
    ts_draw_frame=801_000_000,
    ts_end_draw_frame=802_000_000,
    ts_gpu=None,
    ts_end_gpu=None)

# One more frame after the CUJ is finished
add_frame(
    trace,
    vsync=1000,
    ts_do_frame=1_100_000_000,
    ts_end_do_frame=1_200_000_000,
    ts_draw_frame=1_150_000_000,
    ts_end_draw_frame=1_300_000_000,
    ts_gpu=1_400_000_000,
    ts_end_gpu=1_500_000_000)

add_instant_for_track(
    trace, ts=990_000_000, track=CANCELED_CUJ_TRACK, name="FT#cancel#0")

add_expected_display_frame_events(ts=1_000_000_000, dur=16_000_000, token=10)
add_actual_display_frame_events(ts=1_000_000_000, dur=16_000_000, token=10)

add_expected_surface_frame_events(ts=0, dur=16_000_000, token=10)
add_actual_surface_frame_events(ts=0, dur=16_000_000, token=10)

add_expected_display_frame_events(ts=1_016_000_000, dur=20_000_000, token=20)
add_actual_display_frame_events(ts=1_016_000_000, dur=10_000_000, token=20)

add_expected_surface_frame_events(ts=8_000_000, dur=20_000_000, token=20)
add_actual_surface_frame_events(ts=8_000_000, dur=28_000_000, token=20, jank=66)

add_expected_display_frame_events(ts=1_032_000_000, dur=16_000_000, token=30)
add_actual_display_frame_events(ts=1_032_000_000, dur=16_000_000, token=30)

add_expected_surface_frame_events(ts=30_000_000, dur=20_000_000, token=30)
add_actual_surface_frame_events(
    ts=30_000_000, dur=25_000_000, token=30, jank=64)

add_expected_display_frame_events(ts=1_048_000_000, dur=16_000_000, token=40)
add_actual_display_frame_events(ts=1_048_000_000, dur=16_000_000, token=40)

add_expected_surface_frame_events(ts=40_000_000, dur=20_000_000, token=40)
add_actual_surface_frame_events(
    ts=40_000_000, dur=40_000_000, token=40, jank=64)

add_expected_display_frame_events(ts=1_070_000_000, dur=16_000_000, token=60)
add_actual_display_frame_events(ts=1_070_000_000, dur=16_000_000, token=60)

add_expected_surface_frame_events(ts=70_000_000, dur=20_000_000, token=60)
add_actual_surface_frame_events(
    ts=70_000_000, dur=10_000_000, token=60, jank=64)
add_actual_surface_frame_events(
    ts=70_000_000,
    dur=20_000_000,
    token=60,
    cookie=62,
    jank=64,
    # second layer produced frame later so was picked up by the next SF frame
    display_frame_token_override=190)

add_expected_display_frame_events(ts=1_100_000_000, dur=16_000_000, token=90)
add_actual_display_frame_events(ts=1_100_000_000, dur=16_000_000, token=90)

add_expected_surface_frame_events(ts=100_000_000, dur=20_000_000, token=90)
add_actual_surface_frame_events(
    ts=100_000_000, dur=23_000_000, token=90, jank=64)

add_expected_display_frame_events(ts=1_200_000_000, dur=16_000_000, token=100)
add_actual_display_frame_events(
    ts=1_200_000_000, dur=32_000_000, token=100, jank=16)

add_expected_surface_frame_events(ts=200_000_000, dur=20_000_000, token=100)
add_actual_surface_frame_events(
    ts=200_000_000, dur=22_000_000, token=100, jank=34)

add_expected_surface_frame_events(ts=300_000_000, dur=20_000_000, token=110)
add_actual_surface_frame_events(
    ts=300_000_000, cookie=112, dur=61_000_000, token=110)
add_actual_surface_frame_events(
    ts=300_000_000,
    cookie=114,
    dur=80_000_000,
    token=110,
    jank=64,
    layer_name="TX - JankyLayer#1")

add_expected_surface_frame_events(ts=400_000_000, dur=20_000_000, token=120)
add_actual_surface_frame_events(
    ts=400_000_000,
    dur=61_000_000,
    token=120,
    jank=128,
    on_time_finish_override=1)

# Multiple layers but only one of them janked (the one we care about)
add_expected_surface_frame_events(ts=500_000_000, dur=20_000_000, token=130)
add_actual_surface_frame_events(ts=500_000_000, dur=2_000_000, token=130)
add_actual_surface_frame_events(
    ts=550_000_000, dur=6_000_000, token=130, cookie=132, jank=64)

# Single layer but actual frame event is slighly after doFrame start
add_expected_surface_frame_events(ts=600_000_000, dur=20_000_000, token=140)
add_actual_surface_frame_events(
    ts=608_600_000, dur=17_000_000, token=140, jank=64)

# Surface flinger stuffing frame not classified as missed
add_expected_surface_frame_events(ts=650_000_000, dur=20_000_000, token=145)
add_actual_surface_frame_events(
    ts=650_000_000, dur=20_000_000, token=145, jank=512)

add_expected_surface_frame_events(ts=700_000_000, dur=20_000_000, token=150)
add_actual_surface_frame_events(ts=700_500_000, dur=14_500_000, token=150)

# No matching actual timeline
add_expected_surface_frame_events(ts=800_000_000, dur=20_000_000, token=160)

add_expected_surface_frame_events(ts=1_100_000_000, dur=20_000_000, token=1000)
add_actual_surface_frame_events(
    ts=1_100_000_000, dur=500_000_000, token=1000, jank=64)

sys.stdout.buffer.write(trace.trace.SerializeToString())
