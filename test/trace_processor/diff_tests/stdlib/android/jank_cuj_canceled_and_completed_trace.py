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

import sys
import synth_common

SYSUI_PID = 5000
SYSUI_UI_TID = 5020
SYSUI_RTID = 1555
HWUI_TID = 1556

SYSUI_PACKAGE = "com.android.systemui"
SYSUI_UID = 10001
LAYER_0 = "TX - shade#0"

SHARED_CUJ = "J<SHADE_EXPAND>"
ONLY_CANCELED_CUJ = "J<ONLY_CANCELED>"


def add_expected_surface_frame_events(trace, ts, dur, token, pid):
  trace.add_expected_surface_frame_start_event(
      ts=ts,
      cookie=100000 + token,
      token=token,
      display_frame_token=100 + token,
      pid=pid,
      layer_name="")
  trace.add_frame_end_event(ts=ts + dur, cookie=100000 + token)


def add_actual_surface_frame_events(trace, ts, dur, token, layer, pid):
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
      layer_name=layer)
  trace.add_frame_end_event(ts=ts + dur, cookie=200000 + token)


def setup_trace():
  trace = synth_common.create_trace()
  trace.add_packet()
  trace.add_package_list(
      ts=0, name=SYSUI_PACKAGE, uid=SYSUI_UID, version_code=1)
  trace.add_process(
      pid=SYSUI_PID, ppid=SYSUI_PID, cmdline=SYSUI_PACKAGE, uid=SYSUI_UID)
  trace.add_thread(
      tid=SYSUI_PID, tgid=SYSUI_PID, cmdline="MainThread", name="MainThread")
  trace.add_thread(
      tid=SYSUI_UI_TID, tgid=SYSUI_PID, cmdline="UiThread", name="UiThread")
  trace.add_thread(
      tid=SYSUI_RTID,
      tgid=SYSUI_PID,
      cmdline="RenderThread",
      name="RenderThread")
  trace.add_thread(
      tid=HWUI_TID, tgid=SYSUI_PID, cmdline="hwuiTask0", name="hwuiTask0")
  return trace


def add_canceled_run_of_shared_cuj(trace):
  trace.add_ftrace_packet(cpu=0)
  trace.add_async_atrace_for_thread(
      ts=10_000_000,
      ts_end=40_000_000,
      buf=SHARED_CUJ,
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID)
  trace.add_atrace_instant(
      ts=10_000_001,
      buf=SHARED_CUJ + "#UIThread",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID)
  trace.add_atrace_instant_for_track(
      ts=10_000_002,
      buf="FT#beginVsync#10",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=SHARED_CUJ)
  trace.add_atrace_instant_for_track(
      ts=10_000_010,
      buf="FT#layerId#0",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=SHARED_CUJ)
  trace.add_atrace_instant_for_track(
      ts=35_000_000,
      buf="FT#endVsync#15",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=SHARED_CUJ)
  trace.add_atrace_instant_for_track(
      ts=36_000_000,
      buf="FT#cancel#0",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=SHARED_CUJ)

  trace.add_frame(
      vsync=10,
      ts_do_frame=12_000_000,
      ts_end_do_frame=20_000_000,
      tid=SYSUI_UI_TID,
      pid=SYSUI_PID)
  trace.add_atrace_for_thread(
      ts=13_000_000,
      ts_end=18_000_000,
      buf="binder transaction",
      tid=SYSUI_UI_TID,
      pid=SYSUI_PID)
  trace.add_atrace_for_thread(
      ts=15_000_000,
      ts_end=19_000_000,
      buf="DrawFrames 10",
      tid=SYSUI_RTID,
      pid=SYSUI_PID)

  add_expected_surface_frame_events(
      trace, ts=12_000_000, dur=16_000_000, token=10, pid=SYSUI_PID)
  add_actual_surface_frame_events(
      trace,
      ts=12_000_000,
      dur=10_000_000,
      token=10,
      layer=LAYER_0,
      pid=SYSUI_PID)


def add_completed_run_of_shared_cuj(trace):
  trace.add_ftrace_packet(cpu=0)
  trace.add_async_atrace_for_thread(
      ts=50_000_000,
      ts_end=90_000_000,
      buf=SHARED_CUJ,
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID)
  trace.add_atrace_instant(
      ts=50_000_001,
      buf=SHARED_CUJ + "#UIThread",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID)
  trace.add_atrace_instant_for_track(
      ts=50_000_002,
      buf="FT#beginVsync#20",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=SHARED_CUJ)
  trace.add_atrace_instant_for_track(
      ts=50_000_010,
      buf="FT#layerId#0",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=SHARED_CUJ)
  trace.add_atrace_instant_for_track(
      ts=85_000_000,
      buf="FT#endVsync#25",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=SHARED_CUJ)
  trace.add_atrace_instant_for_track(
      ts=86_000_000,
      buf="FT#end#0",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=SHARED_CUJ)

  trace.add_frame(
      vsync=20,
      ts_do_frame=52_000_000,
      ts_end_do_frame=58_000_000,
      tid=SYSUI_UI_TID,
      pid=SYSUI_PID)
  trace.add_atrace_for_thread(
      ts=53_000_000,
      ts_end=55_000_000,
      buf="animation",
      tid=SYSUI_UI_TID,
      pid=SYSUI_PID)

  # DrawFrames 20 [54ms, 55ms) with nested waitForBufferRelease [54.5ms, 54.7ms)
  trace.add_atrace_begin(
      ts=54_000_000, buf="DrawFrames 20", tid=SYSUI_RTID, pid=SYSUI_PID)
  trace.add_atrace_begin(
      ts=54_500_000, buf="waitForBufferRelease", tid=SYSUI_RTID, pid=SYSUI_PID)
  trace.add_atrace_end(ts=54_700_000, tid=SYSUI_RTID, pid=SYSUI_PID)
  trace.add_atrace_end(ts=55_000_000, tid=SYSUI_RTID, pid=SYSUI_PID)

  add_expected_surface_frame_events(
      trace, ts=52_000_000, dur=16_000_000, token=20, pid=SYSUI_PID)
  add_actual_surface_frame_events(
      trace,
      ts=52_000_000,
      dur=8_000_000,
      token=20,
      layer=LAYER_0,
      pid=SYSUI_PID)


def add_other_excluded_cujs(trace):
  trace.add_ftrace_packet(cpu=0)
  # Canceled-only CUJ
  trace.add_async_atrace_for_thread(
      ts=100_000_000,
      ts_end=120_000_000,
      buf=ONLY_CANCELED_CUJ,
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID)
  trace.add_atrace_instant(
      ts=100_000_001,
      buf=ONLY_CANCELED_CUJ + "#UIThread",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID)
  trace.add_atrace_instant_for_track(
      ts=100_000_002,
      buf="FT#beginVsync#30",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=ONLY_CANCELED_CUJ)
  trace.add_atrace_instant_for_track(
      ts=100_000_010,
      buf="FT#layerId#0",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=ONLY_CANCELED_CUJ)
  trace.add_atrace_instant_for_track(
      ts=115_000_000,
      buf="FT#endVsync#35",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=ONLY_CANCELED_CUJ)
  trace.add_atrace_instant_for_track(
      ts=116_000_000,
      buf="FT#cancel#0",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=ONLY_CANCELED_CUJ)
  trace.add_frame(
      vsync=30,
      ts_do_frame=102_000_000,
      ts_end_do_frame=108_000_000,
      tid=SYSUI_UI_TID,
      pid=SYSUI_PID)
  trace.add_atrace_for_thread(
      ts=103_000_000,
      ts_end=106_000_000,
      buf="layout",
      tid=SYSUI_UI_TID,
      pid=SYSUI_PID)
  trace.add_atrace_for_thread(
      ts=104_000_000,
      ts_end=106_000_000,
      buf="DrawFrames 30",
      tid=SYSUI_RTID,
      pid=SYSUI_PID)
  add_expected_surface_frame_events(
      trace, ts=102_000_000, dur=16_000_000, token=30, pid=SYSUI_PID)
  add_actual_surface_frame_events(
      trace,
      ts=102_000_000,
      dur=8_000_000,
      token=30,
      layer=LAYER_0,
      pid=SYSUI_PID)


def add_sched_events(trace):
  # CPU 1: UI thread sched slices (in both canceled and completed runs)
  trace.add_ftrace_packet(cpu=1)
  trace.add_sched(
      ts=13_000_000, prev_pid=0, next_pid=SYSUI_UI_TID, next_comm="UiThread")
  trace.add_sched(
      ts=18_000_000,
      prev_pid=SYSUI_UI_TID,
      next_pid=0,
      prev_comm="UiThread",
      prev_state="S")
  trace.add_sched(
      ts=52_500_000, prev_pid=0, next_pid=SYSUI_UI_TID, next_comm="UiThread")
  trace.add_sched(
      ts=53_800_000,
      prev_pid=SYSUI_UI_TID,
      next_pid=0,
      prev_comm="UiThread",
      prev_state="S")
  trace.add_sched(
      ts=54_300_000, prev_pid=0, next_pid=SYSUI_UI_TID, next_comm="UiThread")
  trace.add_sched(
      ts=55_500_000,
      prev_pid=SYSUI_UI_TID,
      next_pid=0,
      prev_comm="UiThread",
      prev_state="S")

  # CPU 2: RenderThread sched slices, including [54.7ms, 55.0ms) after
  # waitForBufferRelease [54.5ms, 54.7ms) to exercise un-truncated [ts, ts_end).
  trace.add_ftrace_packet(cpu=2)
  trace.add_sched(
      ts=15_000_000, prev_pid=0, next_pid=SYSUI_RTID, next_comm="RenderThread")
  trace.add_sched(
      ts=19_000_000,
      prev_pid=SYSUI_RTID,
      next_pid=0,
      prev_comm="RenderThread",
      prev_state="S")
  trace.add_sched(
      ts=54_000_000, prev_pid=0, next_pid=SYSUI_RTID, next_comm="RenderThread")
  trace.add_sched(
      ts=54_500_000,
      prev_pid=SYSUI_RTID,
      next_pid=0,
      prev_comm="RenderThread",
      prev_state="S")
  trace.add_sched(
      ts=54_700_000, prev_pid=0, next_pid=SYSUI_RTID, next_comm="RenderThread")
  trace.add_sched(
      ts=55_000_000,
      prev_pid=SYSUI_RTID,
      next_pid=0,
      prev_comm="RenderThread",
      prev_state="S")

  # CPU 3: hwuiTask0 sched slices (2ms in canceled Run 1, 0.3ms in completed Run 2)
  trace.add_ftrace_packet(cpu=3)
  trace.add_sched(
      ts=16_000_000, prev_pid=0, next_pid=HWUI_TID, next_comm="hwuiTask0")
  trace.add_sched(
      ts=18_000_000,
      prev_pid=HWUI_TID,
      next_pid=0,
      prev_comm="hwuiTask0",
      prev_state="S")
  trace.add_sched(
      ts=54_200_000, prev_pid=0, next_pid=HWUI_TID, next_comm="hwuiTask0")
  trace.add_sched(
      ts=54_500_000,
      prev_pid=HWUI_TID,
      next_pid=0,
      prev_comm="hwuiTask0",
      prev_state="S")


trace = setup_trace()
add_canceled_run_of_shared_cuj(trace)
add_completed_run_of_shared_cuj(trace)
add_other_excluded_cujs(trace)
add_sched_events(trace)
sys.stdout.buffer.write(trace.trace.SerializeToString())
