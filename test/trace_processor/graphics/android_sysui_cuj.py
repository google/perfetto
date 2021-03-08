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

from os import sys, path

import synth_common

PID = 1000
RTID = 1555
LAYER = "TX - NotificationShade#0"


def add_main_thread_atrace(trace, ts, ts_end, buf):
  trace.add_atrace_begin(ts=ts, tid=PID, pid=PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=PID, pid=PID)


def add_render_thread_atrace(trace, ts, ts_end, buf):
  trace.add_atrace_begin(ts=ts, tid=RTID, pid=PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=RTID, pid=PID)


def add_gpu_thread_atrace(trace, ts, ts_end, buf):
  trace.add_atrace_begin(ts=ts, tid=1666, pid=PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=1666, pid=PID)


def add_frame(trace, vsync, ts_do_frame, ts_end_do_frame, ts_draw_frame,
              ts_end_draw_frame, ts_gpu, ts_end_gpu):
  add_main_thread_atrace(trace, ts_do_frame, ts_end_do_frame,
                         "Choreographer#doFrame %d" % vsync)
  add_render_thread_atrace(trace, ts_draw_frame, ts_end_draw_frame,
                           "DrawFrames %d" % vsync)
  add_gpu_thread_atrace(trace, ts_gpu, ts_end_gpu,
                        "waiting for GPU completion 123")


def add_display_frame_events(ts, dur, token_start, jank=None):
  jank_type = jank if jank is not None else 1
  present_type = 2 if jank is not None else 1
  on_time_finish = 1 if jank is None else 0
  trace.add_expected_display_frame_start_event(
      ts=ts, cookie=token_start, token=token_start, pid=PID)
  trace.add_frame_end_event(ts=ts + 20_500_000, cookie=token_start)
  trace.add_actual_display_frame_start_event(
      ts=ts,
      cookie=token_start + 1,
      token=token_start,
      pid=PID,
      present_type=present_type,
      on_time_finish=on_time_finish,
      gpu_composition=0,
      jank_type=jank_type,
      prediction_type=3)
  trace.add_frame_end_event(ts=ts + dur, cookie=token_start + 1)
  trace.add_expected_surface_frame_start_event(
      ts=ts,
      cookie=token_start + 2,
      token=token_start + 1,
      display_frame_token=token_start,
      pid=PID,
      layer_name=LAYER)
  trace.add_frame_end_event(ts=ts + 20_500_000, cookie=token_start + 2)
  trace.add_actual_surface_frame_start_event(
      ts=ts,
      cookie=token_start + 3,
      token=token_start + 1,
      display_frame_token=token_start,
      pid=PID,
      layer_name=LAYER,
      present_type=present_type,
      on_time_finish=on_time_finish,
      gpu_composition=0,
      jank_type=jank_type,
      prediction_type=3)
  trace.add_frame_end_event(ts=ts + dur, cookie=token_start + 3)


trace = synth_common.create_trace()

trace.add_packet()
trace.add_package_list(
    ts=0, name="com.android.systemui", uid=10001, version_code=1)

trace.add_process(pid=PID, ppid=1, cmdline="com.android.systemui", uid=10001)
trace.add_thread(
    tid=RTID, tgid=PID, cmdline="RenderThread", name="RenderThread")
trace.add_thread(
    tid=1666, tgid=PID, cmdline="GPU completion", name="GPU completion")

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_async_begin(ts=0, tid=PID, pid=PID, buf="J<SHADE_ROW_EXPAND>")
trace.add_atrace_async_end(
    ts=1_000_000_000, tid=PID, pid=PID, buf="J<SHADE_ROW_EXPAND>")

add_frame(
    trace,
    vsync=1,
    ts_do_frame=0,
    ts_end_do_frame=5_000_000,
    ts_draw_frame=4_000_000,
    ts_end_draw_frame=5_000_000,
    ts_gpu=10_000_000,
    ts_end_gpu=15_000_000)
add_main_thread_atrace(
    trace, ts=1_500_000, ts_end=2_000_000, buf="binder transaction")
add_render_thread_atrace(
    trace, ts=4_500_000, ts_end=4_800_000, buf="flush layers")


add_frame(
    trace,
    vsync=2,
    ts_do_frame=8_000_000,
    ts_end_do_frame=23_000_000,
    ts_draw_frame=22_000_000,
    ts_end_draw_frame=26_000_000,
    ts_gpu=27_500_000,
    ts_end_gpu=35_000_000)
add_main_thread_atrace(
    trace, ts=9_000_000, ts_end=20_000_000, buf="binder transaction")
add_render_thread_atrace(
    trace, ts=24_000_000, ts_end=25_000_000, buf="flush layers")

add_frame(
    trace,
    vsync=3,
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

add_frame(
    trace,
    vsync=4,
    ts_do_frame=40_000_000,
    ts_end_do_frame=53_000_000,
    ts_draw_frame=52_000_000,
    ts_end_draw_frame=59_000_000,
    ts_gpu=66_500_000,
    ts_end_gpu=78_000_000)

# Main thread Running for 14 millis
trace.add_sched(ts=39_000_000, prev_pid=0, next_pid=PID)
trace.add_sched(ts=53_000_000, prev_pid=PID, next_pid=0, prev_state='R')

# RenderThread Running for 5 millis
trace.add_sched(ts=54_000_000, prev_pid=0, next_pid=RTID)
trace.add_sched(ts=59_000_000, prev_pid=RTID, next_pid=0, prev_state='R')

add_frame(
    trace,
    vsync=6,
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

add_frame(
    trace,
    vsync=9,
    ts_do_frame=100_000_000,
    ts_end_do_frame=115_000_000,
    ts_draw_frame=102_000_000,
    ts_end_draw_frame=104_000_000,
    ts_gpu=108_000_000,
    ts_end_gpu=115_600_000)

add_render_thread_atrace(
    trace, ts=108_000_000, ts_end=114_000_000, buf="DrawFrames 6")
add_gpu_thread_atrace(
    trace,
    ts=121_500_000,
    ts_end=122_000_000,
    buf="waiting for GPU completion 123")

add_frame(
    trace,
    vsync=10,
    ts_do_frame=200_000_000,
    ts_end_do_frame=215_000_000,
    ts_draw_frame=202_000_000,
    ts_end_draw_frame=204_000_000,
    ts_gpu=208_000_000,
    ts_end_gpu=210_000_000)

add_render_thread_atrace(
    trace, ts=208_000_000, ts_end=214_000_000, buf="DrawFrames 7")

add_frame(
    trace,
    vsync=11,
    ts_do_frame=300_000_000,
    ts_end_do_frame=315_000_000,
    ts_draw_frame=302_000_000,
    ts_end_draw_frame=304_000_000,
    ts_gpu=308_000_000,
    ts_end_gpu=310_000_000)

add_render_thread_atrace(
    trace, ts=305_000_000, ts_end=308_000_000, buf="dispatchFrameCallbacks")

# One more frame after the CUJ is finished
add_frame(
    trace,
    vsync=13,
    ts_do_frame=1_100_000_000,
    ts_end_do_frame=1_200_000_000,
    ts_draw_frame=1_150_000_000,
    ts_end_draw_frame=1_300_000_000,
    ts_gpu=1_400_000_000,
    ts_end_gpu=1_500_000_000)

add_display_frame_events(ts=0, dur=16_000_000, token_start=10)
add_display_frame_events(ts=8_000_000, dur=28_000_000, token_start=20, jank=66)
add_display_frame_events(ts=30_000_000, dur=25_000_000, token_start=30, jank=64)
add_display_frame_events(ts=40_000_000, dur=40_000_000, token_start=40, jank=64)
add_display_frame_events(ts=70_000_000, dur=20_000_000, token_start=50, jank=64)
add_display_frame_events(
    ts=100_000_000, dur=23_000_000, token_start=60, jank=64)
add_display_frame_events(
    ts=200_000_000, dur=12_000_000, token_start=70, jank=34)
add_display_frame_events(ts=300_000_000, dur=61_000_000, token_start=80)
add_display_frame_events(
    ts=1_100_000_000, dur=500_000_000, token_start=100, jank=64)

sys.stdout.buffer.write(trace.trace.SerializeToString())
