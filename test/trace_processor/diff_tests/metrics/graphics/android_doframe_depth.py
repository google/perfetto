#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

PROCESS_TRACK = 1234


def add_render_thread_atrace_begin(trace, ts, buf):
  trace.add_atrace_begin(ts=ts, tid=RTID, pid=PID, buf=buf)


def add_render_thread_atrace_end(trace, ts_end):
  trace.add_atrace_end(ts=ts_end, tid=RTID, pid=PID)


def add_render_thread_atrace(trace, ts, ts_end, buf):
  add_render_thread_atrace_begin(trace, ts, buf)
  add_render_thread_atrace_end(trace, ts_end)


def add_gpu_thread_atrace(trace, ts, ts_end, buf):
  trace.add_atrace_begin(ts=ts, tid=1666, pid=PID, buf=buf)


def add_main_thread_atrace_from_depth(trace, ts, ts_end, buf, depth=0):
  for i in range(0, depth):
    trace.add_atrace_begin(ts=ts - (depth - i), tid=PID, pid=PID, buf='<depth %d>'.format(i))
  trace.add_atrace_begin(ts=ts, tid=PID, pid=PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=PID, pid=PID)
  for i in range(0, depth):
    trace.add_atrace_end(ts=ts_end + (i + 1), tid=PID, pid=PID)


def add_frame_from_depth(trace,
              vsync,
              ts_do_frame,
              ts_end_do_frame,
              ts_draw_frame,
              ts_end_draw_frame,
              ts_gpu=None,
              ts_end_gpu=None,
              resync=False,
              depth=0):
  add_main_thread_atrace_from_depth(trace, ts_do_frame, ts_end_do_frame,
                         "Choreographer#doFrame %d" % vsync, depth)
  if resync:
    add_main_thread_atrace_from_depth(
        trace, ts_do_frame, ts_end_do_frame,
        "Choreographer#doFrame - resynced to %d in 0.0s" % (vsync + 1), depth)
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


trace = synth_common.create_trace()

trace.add_packet()
trace.add_package_list(
    ts=0, name="com.android.systemui", uid=10001, version_code=1)

trace.add_process(pid=PID, ppid=1, cmdline="com.android.systemui", uid=10001)
trace.add_thread(
    tid=RTID, tgid=PID, cmdline="RenderThread", name="RenderThread")
trace.add_process_track_descriptor(PROCESS_TRACK, pid=PID)

trace.add_ftrace_packet(cpu=0)
add_frame_from_depth(
    trace,
    vsync=10,
    ts_do_frame=1_000_000,
    ts_end_do_frame=5_000_000,
    ts_draw_frame=5_000_000,
    ts_end_draw_frame=10_000_000,
    depth=0)

add_frame_from_depth(
    trace,
    vsync=11,
    ts_do_frame=30_000_000,
    ts_end_do_frame=35_000_000,
    ts_draw_frame=33_000_000,
    ts_end_draw_frame=38_000_000,
    depth=1)

add_frame_from_depth(
    trace,
    vsync=12,
    ts_do_frame=60_000_000,
    ts_end_do_frame=65_000_000,
    ts_draw_frame=65_00_000,
    ts_end_draw_frame=70_000_000,
    depth=2)

add_frame_from_depth(
    trace,
    vsync=13,
    ts_do_frame=90_000_000,
    ts_end_do_frame=98_000_000,
    ts_draw_frame=96_000_000,
    ts_end_draw_frame=102_000_000,
    ts_gpu=100_000_000,
    ts_end_gpu=115_000_000,
    depth=3)

sys.stdout.buffer.write(trace.trace.SerializeToString())
