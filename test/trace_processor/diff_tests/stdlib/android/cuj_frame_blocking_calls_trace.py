#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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

# Synthetic trace with:
# 1. A completed jank CUJ (`J<COMPLETED_CUJ>`) with two `hwuiTask*` threads
#    (`hwuiTask0` and `hwuiTask1`) both running during `DrawFrames 20` so they
#    appear as separate interval rows (`id`) sharing one logical `call_id`.
# 2. A canceled jank CUJ (`J<CANCELED_CUJ>`) whose frames and blocking calls
#    must be excluded from `android_cuj_frame_boundaries` and
#    `android_cuj_frame_blocking_calls`.

import sys
import synth_common

SYSUI_PID = 5000
SYSUI_UI_TID = 5020
SYSUI_RTID = 1555
HWUI_TID_0 = 1556
HWUI_TID_1 = 1557
LAYER_1 = "TX - first_layer#0"


def add_frame_timeline(trace, ts, dur, token, pid):
  trace.add_expected_surface_frame_start_event(
      ts=ts,
      cookie=100000 + token,
      token=token,
      display_frame_token=100 + token,
      pid=pid,
      layer_name="")
  trace.add_frame_end_event(ts=ts + dur, cookie=100000 + token)
  trace.add_actual_surface_frame_start_event(
      ts=ts,
      cookie=200000 + token,
      token=token,
      display_frame_token=100 + token,
      pid=pid,
      present_type=1,
      on_time_finish=1,
      gpu_composition=0,
      jank_type=1,
      prediction_type=3,
      layer_name=LAYER_1)
  trace.add_frame_end_event(ts=ts + dur, cookie=200000 + token)


trace = synth_common.create_trace()
trace.add_package_list(
    ts=0, name="com.android.systemui", uid=10001, version_code=1)
trace.add_packet()
trace.add_process(
    pid=SYSUI_PID, ppid=SYSUI_PID, cmdline="com.android.systemui", uid=10001)
trace.add_thread(
    tid=SYSUI_PID, tgid=SYSUI_PID, cmdline="MainThread", name="MainThread")
trace.add_thread(
    tid=SYSUI_UI_TID, tgid=SYSUI_PID, cmdline="UiThread", name="UiThread")
trace.add_thread(
    tid=SYSUI_RTID, tgid=SYSUI_PID, cmdline="RenderThread", name="RenderThread")
trace.add_thread(
    tid=HWUI_TID_0, tgid=SYSUI_PID, cmdline="hwuiTask0", name="hwuiTask0")
trace.add_thread(
    tid=HWUI_TID_1, tgid=SYSUI_PID, cmdline="hwuiTask1", name="hwuiTask1")

trace.add_ftrace_packet(cpu=0)

# Completed CUJ [25ms, 50ms] with frame 20.
completed_cuj = "J<COMPLETED_CUJ>"
trace.add_async_atrace_for_thread(
    ts=25_000_000,
    ts_end=50_000_000,
    buf=completed_cuj,
    pid=SYSUI_PID,
    tid=SYSUI_UI_TID)
trace.add_atrace_instant(
    ts=25_000_001,
    buf=completed_cuj + "#UIThread",
    pid=SYSUI_PID,
    tid=SYSUI_UI_TID)
trace.add_atrace_instant_for_track(
    ts=25_000_002,
    buf="FT#beginVsync#20",
    pid=SYSUI_PID,
    tid=SYSUI_UI_TID,
    track_name=completed_cuj)
trace.add_atrace_instant_for_track(
    ts=25_000_010,
    buf="FT#layerId#0",
    pid=SYSUI_PID,
    tid=SYSUI_UI_TID,
    track_name=completed_cuj)
trace.add_atrace_instant_for_track(
    ts=49_000_000,
    buf="FT#endVsync#30",
    pid=SYSUI_PID,
    tid=SYSUI_UI_TID,
    track_name=completed_cuj)

trace.add_frame(
    vsync=20,
    ts_do_frame=26_000_000,
    ts_end_do_frame=32_000_000,
    tid=SYSUI_UI_TID,
    pid=SYSUI_PID)
trace.add_atrace_for_thread(
    ts=27_000_000,
    ts_end=30_000_000,
    buf="DrawFrames 20",
    tid=SYSUI_RTID,
    pid=SYSUI_PID)
trace.add_atrace_for_thread(
    ts=28_000_000,
    ts_end=29_000_000,
    buf="animation",
    tid=SYSUI_UI_TID,
    pid=SYSUI_PID)

# Two hwuiTask* running intervals during DrawFrames 20 [27ms, 30ms].
trace.add_sched(
    ts=27_200_000, prev_pid=0, next_pid=HWUI_TID_0, next_comm="hwuiTask0")
trace.add_sched(
    ts=27_700_000,
    prev_pid=HWUI_TID_0,
    next_pid=HWUI_TID_1,
    prev_comm="hwuiTask0",
    prev_state="S",
    next_comm="hwuiTask1")
trace.add_sched(
    ts=28_500_000,
    prev_pid=HWUI_TID_1,
    next_pid=0,
    prev_comm="hwuiTask1",
    prev_state="S")

add_frame_timeline(trace, ts=27_000_000, dur=7_000_000, token=20, pid=SYSUI_PID)

# Canceled CUJ [60ms, 80ms] with frame 40 and FT#cancel marker.
trace.add_ftrace_packet(cpu=0)
canceled_cuj = "J<CANCELED_CUJ>"
trace.add_async_atrace_for_thread(
    ts=60_000_000,
    ts_end=80_000_000,
    buf=canceled_cuj,
    pid=SYSUI_PID,
    tid=SYSUI_UI_TID)
trace.add_atrace_instant(
    ts=60_000_001,
    buf=canceled_cuj + "#UIThread",
    pid=SYSUI_PID,
    tid=SYSUI_UI_TID)
trace.add_atrace_instant_for_track(
    ts=60_000_002,
    buf="FT#beginVsync#40",
    pid=SYSUI_PID,
    tid=SYSUI_UI_TID,
    track_name=canceled_cuj)
trace.add_atrace_instant_for_track(
    ts=60_000_010,
    buf="FT#layerId#0",
    pid=SYSUI_PID,
    tid=SYSUI_UI_TID,
    track_name=canceled_cuj)
trace.add_atrace_instant_for_track(
    ts=79_000_000,
    buf="FT#cancel#0",
    pid=SYSUI_PID,
    tid=SYSUI_UI_TID,
    track_name=canceled_cuj)

trace.add_frame(
    vsync=40,
    ts_do_frame=61_000_000,
    ts_end_do_frame=67_000_000,
    tid=SYSUI_UI_TID,
    pid=SYSUI_PID)
trace.add_atrace_for_thread(
    ts=62_000_000,
    ts_end=64_000_000,
    buf="DrawFrames 40",
    tid=SYSUI_RTID,
    pid=SYSUI_PID)
trace.add_atrace_for_thread(
    ts=63_000_000,
    ts_end=65_000_000,
    buf="animation",
    tid=SYSUI_UI_TID,
    pid=SYSUI_PID)
trace.add_sched(
    ts=62_500_000, prev_pid=0, next_pid=HWUI_TID_0, next_comm="hwuiTask0")
trace.add_sched(
    ts=63_000_000,
    prev_pid=HWUI_TID_0,
    next_pid=0,
    prev_comm="hwuiTask0",
    prev_state="S")
add_frame_timeline(trace, ts=62_000_000, dur=7_000_000, token=40, pid=SYSUI_PID)

sys.stdout.buffer.write(trace.trace.SerializeToString())
