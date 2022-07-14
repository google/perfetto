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

from lib2to3.pgen2 import token
from os import sys, path

import synth_common

PID = 1000
RTID = 1555
JITID = 1777
LAYER = "TX - NotificationShade#0"


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


def add_frame(trace,
              vsync,
              ts_do_frame,
              ts_end_do_frame,
              ts_draw_frame,
              ts_end_draw_frame,
              ts_gpu=None,
              ts_end_gpu=None):
  add_main_thread_atrace(trace, ts_do_frame, ts_end_do_frame,
                         "Choreographer#doFrame %d" % vsync)

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


def add_expected_frame_events(ts, dur, token_start):
  trace.add_expected_display_frame_start_event(
      ts=ts, cookie=token_start, token=token_start, pid=PID)
  trace.add_frame_end_event(ts=ts + dur, cookie=token_start)


def add_actual_frame_events(ts,
                            dur,
                            token_start,
                            cookie=None,
                            jank=None,
                            on_time_finish_override=None):
  if cookie is None:
    cookie = token_start + 1
  jank_type = jank if jank is not None else 1
  present_type = 2 if jank is not None else 1
  if on_time_finish_override is None:
    on_time_finish = 1 if jank is None else 0
  else:
    on_time_finish = on_time_finish_override
  trace.add_actual_display_frame_start_event(
      ts=ts,
      cookie=cookie,
      token=token_start,
      pid=PID,
      present_type=present_type,
      on_time_finish=on_time_finish,
      gpu_composition=0,
      jank_type=jank_type,
      prediction_type=3)
  trace.add_frame_end_event(ts=ts + dur, cookie=cookie)


trace = synth_common.create_trace()

trace.add_packet()
trace.add_package_list(
    ts=0, name="com.android.systemui", uid=10001, version_code=1)

trace.add_process(pid=PID, ppid=1, cmdline="com.android.systemui", uid=10001)
trace.add_thread(
    tid=RTID, tgid=PID, cmdline="RenderThread", name="RenderThread")
trace.add_thread(
    tid=1666, tgid=PID, cmdline="GPU completion", name="GPU completion")
trace.add_thread(
    tid=JITID, tgid=PID, cmdline="Jit thread pool", name="Jit thread pool")
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_async_begin(ts=5, tid=PID, pid=PID, buf="J<SHOULD_BE_IGNORED>")
trace.add_atrace_async_begin(ts=10, tid=PID, pid=PID, buf="J<SHADE_ROW_EXPAND>")
trace.add_atrace_async_end(
    ts=100_000_000, tid=PID, pid=PID, buf="J<SHOULD_BE_IGNORED>")
trace.add_atrace_async_begin(
    ts=100_100_000, tid=PID, pid=PID, buf="J<CANCELED>")
trace.add_atrace_async_end(
    ts=901_000_010, tid=PID, pid=PID, buf="J<SHADE_ROW_EXPAND>")
trace.add_atrace_async_end(ts=999_000_000, tid=PID, pid=PID, buf="J<CANCELED>")

add_frame(
    trace,
    vsync=10,
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
# Skipped in `android_jank_cuj.sql` since we assume the process did not draw anything.
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

add_main_thread_atrace(
    trace, ts=990_000_000, ts_end=995_000_000, buf="J<CANCELED>#FT#cancel#0")

add_expected_frame_events(ts=0, dur=16_000_000, token_start=10)
add_actual_frame_events(ts=0, dur=16_000_000, token_start=10)

add_expected_frame_events(ts=8_000_000, dur=20_000_000, token_start=20)
add_actual_frame_events(ts=8_000_000, dur=28_000_000, token_start=20, jank=66)

add_expected_frame_events(ts=30_000_000, dur=20_000_000, token_start=30)
add_actual_frame_events(ts=30_000_000, dur=25_000_000, token_start=30, jank=64)

add_expected_frame_events(ts=40_000_000, dur=20_000_000, token_start=40)
add_actual_frame_events(ts=40_000_000, dur=40_000_000, token_start=40, jank=64)

add_expected_frame_events(ts=70_000_000, dur=20_000_000, token_start=60)
add_actual_frame_events(ts=70_000_000, dur=10_000_000, token_start=60, jank=64)
add_actual_frame_events(
    ts=70_000_000, dur=20_000_000, token_start=60, cookie=62, jank=64)

add_expected_frame_events(ts=100_000_000, dur=20_000_000, token_start=90)
add_actual_frame_events(ts=100_000_000, dur=23_000_000, token_start=90, jank=64)

add_expected_frame_events(ts=200_000_000, dur=20_000_000, token_start=100)
add_actual_frame_events(
    ts=200_000_000, dur=22_000_000, token_start=100, jank=34)

add_expected_frame_events(ts=300_000_000, dur=20_000_000, token_start=110)
add_actual_frame_events(ts=300_000_000, dur=61_000_000, token_start=110)

add_expected_frame_events(ts=400_000_000, dur=20_000_000, token_start=120)
add_actual_frame_events(
    ts=400_000_000,
    dur=61_000_000,
    token_start=120,
    jank=128,
    on_time_finish_override=1)

# Multiple layers but only one of them janked (the one we care about)
add_expected_frame_events(ts=500_000_000, dur=20_000_000, token_start=130)
add_actual_frame_events(ts=500_000_000, dur=2_000_000, token_start=130)
add_actual_frame_events(
    ts=550_000_000, dur=6_000_000, token_start=130, cookie=132, jank=64)

# Single layer but actual frame event is slighly after doFrame start
add_expected_frame_events(ts=600_000_000, dur=20_000_000, token_start=140)
add_actual_frame_events(
    ts=608_600_000, dur=17_000_000, token_start=140, jank=64)

add_expected_frame_events(ts=700_000_000, dur=20_000_000, token_start=150)
add_actual_frame_events(ts=700_500_000, dur=14_500_000, token_start=150)

# No matching actual timeline
add_expected_frame_events(ts=800_000_000, dur=20_000_000, token_start=160)

add_expected_frame_events(ts=1_100_000_000, dur=20_000_000, token_start=1000)
add_actual_frame_events(
    ts=1_100_000_000, dur=500_000_000, token_start=1000, jank=64)

sys.stdout.buffer.write(trace.trace.SerializeToString())
