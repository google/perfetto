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


def add_main_thread_atrace(trace, ts, ts_end, buf):
  trace.add_atrace_begin(ts=ts, tid=PID, pid=PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=PID, pid=PID)


def add_render_thread_atrace(trace, ts, ts_end, buf):
  trace.add_atrace_begin(ts=ts, tid=RTID, pid=PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=RTID, pid=PID)


def add_gpu_thread_atrace(trace, ts, ts_end, buf):
  trace.add_atrace_begin(ts=ts, tid=1666, pid=PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=1666, pid=PID)


def add_frame(trace, ts_do_frame, ts_end_do_frame, ts_draw_frame,
              ts_end_draw_frame, ts_gpu, ts_end_gpu):
  add_main_thread_atrace(trace, ts_do_frame, ts_end_do_frame,
                         "Choreographer#doFrame")
  add_render_thread_atrace(trace, ts_draw_frame, ts_end_draw_frame, "DrawFrame")
  add_gpu_thread_atrace(trace, ts_gpu, ts_end_gpu,
                        "waiting for GPU completion 123")


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
    ts_do_frame=100_000_000,
    ts_end_do_frame=115_000_000,
    ts_draw_frame=102_000_000,
    ts_end_draw_frame=104_000_000,
    ts_gpu=108_000_000,
    ts_end_gpu=115_600_000)

add_render_thread_atrace(
    trace, ts=108_000_000, ts_end=114_000_000, buf="DrawFrame")
add_gpu_thread_atrace(
    trace,
    ts=121_500_000,
    ts_end=122_000_000,
    buf="waiting for GPU completion 123")

add_frame(
    trace,
    ts_do_frame=200_000_000,
    ts_end_do_frame=215_000_000,
    ts_draw_frame=202_000_000,
    ts_end_draw_frame=204_000_000,
    ts_gpu=208_000_000,
    ts_end_gpu=210_000_000)

add_render_thread_atrace(
    trace, ts=208_000_000, ts_end=214_000_000, buf="DrawFrame")

add_frame(
    trace,
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
    ts_do_frame=1_100_000_000,
    ts_end_do_frame=1_200_000_000,
    ts_draw_frame=1_150_000_000,
    ts_end_draw_frame=1_300_000_000,
    ts_gpu=1_400_000_000,
    ts_end_gpu=1_500_000_000)

sys.stdout.buffer.write(trace.trace.SerializeToString())
