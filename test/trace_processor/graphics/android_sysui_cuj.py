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


def add_main_thread_atrace(trace, ts, ts_end, buf):
  trace.add_atrace_begin(ts=ts, tid=PID, pid=PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=PID, pid=PID)


def add_render_thread_atrace(trace, ts, ts_end, buf):
  trace.add_atrace_begin(ts=ts, tid=1555, pid=PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=1555, pid=PID)


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
    tid=1555, tgid=PID, cmdline="RenderThread", name="RenderThread")
trace.add_thread(
    tid=1666, tgid=PID, cmdline="GPU completion", name="GPU completion")

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_async_begin(ts=0, tid=PID, pid=PID, buf="Cuj<5>")
trace.add_atrace_async_end(ts=1_000_000_000, tid=PID, pid=PID, buf="Cuj<5>")

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
    ts_draw_frame=34_000_000,
    ts_end_draw_frame=56_000_000,
    ts_gpu=57_500_000,
    ts_end_gpu=58_000_000)
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
    trace, ts=40_000_000, ts_end=52_000_000, buf="flush layers")

sys.stdout.buffer.write(trace.trace.SerializeToString())
