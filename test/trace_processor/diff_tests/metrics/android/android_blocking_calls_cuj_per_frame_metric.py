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

import synth_common
from os import sys

SYSUI_PID = 5000
SYSUI_UI_TID = 5020
LAUNCHER_PID = 6000
LAUNCHER_UI_TID = 6020

# RenderThread
SYSUI_RTID = 1555
LAUNCHER_RTID = 1655

SYSUI_PACKAGE = "com.android.systemui"
LAUNCHER_PACKAGE = "com.google.android.apps.nexuslauncher"

SYSUI_UID = 10001
LAUNCHER_UID = 10002

LAYER_1 = "TX - first_layer#0"
LAYER_2 = "TX - second_layer#1"
LAYER_3 = "TX - third_layer#2"

FIRST_CUJ = "J<BACK_PANEL_ARROW>"
SECOND_CUJ = "J<CUJ_NAME>"


def add_expected_surface_frame_events(ts, dur, token, pid):
  trace.add_expected_surface_frame_start_event(
      ts=ts,
      cookie=100000 + token,
      token=token,
      display_frame_token=100 + token,
      pid=pid,
      layer_name='')
  trace.add_frame_end_event(ts=ts + dur, cookie=100000 + token)


def add_actual_surface_frame_events(ts, dur, token, layer, pid):
  cookie = token + 1
  trace.add_actual_surface_frame_start_event(
      ts=ts,
      cookie=100002 + cookie,
      token=token,
      display_frame_token=token + 100,
      pid=pid,
      present_type=1,
      on_time_finish=1,
      gpu_composition=0,
      jank_type=1,
      prediction_type=3,
      layer_name=layer)
  trace.add_frame_end_event(ts=ts + dur, cookie=100002 + cookie)


def add_blocking_calls_per_frame_multiple_cuj_instance(trace, cuj_name):

  # add a new CUJ in trace.
  trace.add_async_atrace_for_thread(
      ts=25_000_000,
      ts_end=77_000_000,
      buf=cuj_name,
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID)
  trace.add_async_atrace_for_thread(
      ts=83_000_000,
      ts_end=102_000_000,
      buf=cuj_name,
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID)

  trace.add_atrace_instant(
      ts=25_000_001,
      buf=cuj_name + "#UIThread",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID)

  trace.add_atrace_instant(
      ts=83_000_001,
      buf=cuj_name + "#UIThread",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID)

  trace.add_atrace_instant_for_track(
      ts=25_000_001,
      buf="FT#beginVsync#20",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=cuj_name)

  trace.add_atrace_instant_for_track(
      ts=25_000_010,
      buf="FT#layerId#0",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=cuj_name)

  trace.add_atrace_instant_for_track(
      ts=76_000_001,
      buf="FT#endVsync#30",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=cuj_name)

  trace.add_atrace_instant_for_track(
      ts=83_000_001,
      buf="FT#beginVsync#60",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=cuj_name)

  trace.add_atrace_instant_for_track(
      ts=83_000_010,
      buf="FT#layerId#2",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=cuj_name)

  trace.add_atrace_instant_for_track(
      ts=101_000_001,
      buf="FT#endVsync#70",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=cuj_name)

  # Add Choreographer#doFrame outside CUJ boundary. This frame will not be considered during
  # metric calculation.

  trace.add_frame(
      vsync=15,
      ts_do_frame=9_000_000,
      ts_end_do_frame=15_000_000,
      tid=SYSUI_UI_TID,
      pid=SYSUI_PID)

  trace.add_atrace_for_thread(
      ts=10_000_000,
      ts_end=12_000_000,
      buf="DrawFrames 15",
      tid=SYSUI_RTID,
      pid=SYSUI_PID)

  # Add Choreographer#doFrame slices within CUJ boundary.
  trace.add_frame(
      vsync=20,
      ts_do_frame=26_000_000,
      ts_end_do_frame=32_000_000,
      tid=SYSUI_UI_TID,
      pid=SYSUI_PID)

  trace.add_atrace_for_thread(
      ts=27_000_000,
      ts_end=28_000_000,
      buf="DrawFrames 20",
      tid=SYSUI_RTID,
      pid=SYSUI_PID)

  trace.add_frame(
      vsync=22,
      ts_do_frame=43_000_000,
      ts_end_do_frame=49_000_000,
      tid=SYSUI_UI_TID,
      pid=SYSUI_PID)

  trace.add_atrace_for_thread(
      ts=44_000_000,
      ts_end=45_000_000,
      buf="DrawFrames 22",
      tid=SYSUI_RTID,
      pid=SYSUI_PID)

  trace.add_frame(
      vsync=24,
      ts_do_frame=60_000_000,
      ts_end_do_frame=65_000_000,
      tid=SYSUI_UI_TID,
      pid=SYSUI_PID)

  trace.add_atrace_for_thread(
      ts=61_000_000,
      ts_end=62_000_000,
      buf="DrawFrames 24",
      tid=SYSUI_RTID,
      pid=SYSUI_PID)

  trace.add_frame(
      vsync=65,
      ts_do_frame=84_000_000,
      ts_end_do_frame=89_000_000,
      tid=SYSUI_UI_TID,
      pid=SYSUI_PID)

  trace.add_atrace_for_thread(
      ts=85_000_000,
      ts_end=86_000_000,
      buf="DrawFrames 65",
      tid=SYSUI_RTID,
      pid=SYSUI_PID)

  trace.add_atrace_begin(
      ts=27_000_000, buf="binder transaction", tid=SYSUI_UI_TID, pid=SYSUI_PID)
  trace.add_atrace_end(ts=27_500_000, tid=SYSUI_UI_TID, pid=SYSUI_PID)

  trace.add_atrace_begin(
      ts=27_500_000, buf="binder transaction", tid=SYSUI_UI_TID, pid=SYSUI_PID)
  trace.add_atrace_end(ts=30_000_000, tid=SYSUI_UI_TID, pid=SYSUI_PID)

  trace.add_atrace_begin(
      ts=30_000_000, buf="animation", tid=SYSUI_UI_TID, pid=SYSUI_PID)
  trace.add_atrace_end(ts=32_000_000, tid=SYSUI_UI_TID, pid=SYSUI_PID)

  trace.add_atrace_begin(
      ts=62_000_000, buf="animation", tid=SYSUI_UI_TID, pid=SYSUI_PID)
  trace.add_atrace_end(ts=64_000_000, tid=SYSUI_UI_TID, pid=SYSUI_PID)

  trace.add_atrace_begin(
      ts=86_000_000, buf="binder transaction", tid=SYSUI_UI_TID, pid=SYSUI_PID)
  trace.add_atrace_end(ts=88_500_000, tid=SYSUI_UI_TID, pid=SYSUI_PID)

  # Add expected and actual frames.
  add_expected_surface_frame_events(
      ts=10_000_000, dur=16_000_000, token=15, pid=SYSUI_PID)
  add_actual_surface_frame_events(
      ts=10_000_000, dur=16_000_000, token=15, layer=LAYER_1, pid=SYSUI_PID)

  add_expected_surface_frame_events(
      ts=27_000_000, dur=16_000_000, token=20, pid=SYSUI_PID)
  add_actual_surface_frame_events(
      ts=27_000_000, dur=7_000_000, token=20, layer=LAYER_1, pid=SYSUI_PID)

  add_expected_surface_frame_events(
      ts=44_000_000, dur=16_000_000, token=22, pid=SYSUI_PID)
  add_actual_surface_frame_events(
      ts=44_000_000, dur=7_000_000, token=22, layer=LAYER_1, pid=SYSUI_PID)

  add_expected_surface_frame_events(
      ts=61_000_000, dur=16_000_000, token=24, pid=SYSUI_PID)
  add_actual_surface_frame_events(
      ts=61_000_000, dur=6_000_000, token=24, layer=LAYER_1, pid=SYSUI_PID)

  add_expected_surface_frame_events(
      ts=85_000_000, dur=16_000_000, token=65, pid=SYSUI_PID)
  add_actual_surface_frame_events(
      ts=85_000_000, dur=6_000_000, token=65, layer=LAYER_3, pid=SYSUI_PID)


def add_blocking_call_crossing_frame_boundary(trace, cuj_name):

  # add a new CUJ in trace.
  trace.add_async_atrace_for_thread(
      ts=120_000_000,
      ts_end=145_000_000,
      buf=cuj_name,
      pid=LAUNCHER_PID,
      tid=LAUNCHER_UI_TID)

  trace.add_atrace_instant(
      ts=120_000_001,
      buf=cuj_name + "#UIThread",
      pid=LAUNCHER_PID,
      tid=LAUNCHER_UI_TID)

  trace.add_atrace_instant_for_track(
      ts=120_000_002,
      buf="FT#beginVsync#80",
      pid=LAUNCHER_PID,
      tid=LAUNCHER_UI_TID,
      track_name=cuj_name)

  trace.add_atrace_instant_for_track(
      ts=120_000_010,
      buf="FT#layerId#1",
      pid=LAUNCHER_PID,
      tid=LAUNCHER_UI_TID,
      track_name=cuj_name)

  trace.add_atrace_instant_for_track(
      ts=144_000_001,
      buf="FT#endVsync#90",
      pid=LAUNCHER_PID,
      tid=LAUNCHER_UI_TID,
      track_name=cuj_name)

  # Add Choreographer#doFrame outside CUJ boundary. This frame will not be considered during
  # metric calculation.
  trace.add_frame(
      vsync=75,
      ts_do_frame=103_000_000,
      ts_end_do_frame=110_000_000,
      tid=LAUNCHER_UI_TID,
      pid=LAUNCHER_PID)

  trace.add_atrace_for_thread(
      ts=104_000_000,
      ts_end=105_000_000,
      buf="DrawFrames 75",
      tid=LAUNCHER_RTID,
      pid=LAUNCHER_PID)

  # Add Choreographer#doFrame slices within CUJ boundary.
  trace.add_frame(
      vsync=80,
      ts_do_frame=120_000_000,
      ts_end_do_frame=126_000_000,
      tid=LAUNCHER_UI_TID,
      pid=LAUNCHER_PID)

  trace.add_atrace_for_thread(
      ts=121_000_000,
      ts_end=122_000_000,
      buf="DrawFrames 80",
      tid=LAUNCHER_RTID,
      pid=LAUNCHER_PID)

  trace.add_frame(
      vsync=82,
      ts_do_frame=141_000_000,
      ts_end_do_frame=143_000_000,
      tid=LAUNCHER_UI_TID,
      pid=LAUNCHER_PID)

  trace.add_atrace_for_thread(
      ts=142_000_000,
      ts_end=142_500_000,
      buf="DrawFrames 82",
      tid=LAUNCHER_RTID,
      pid=LAUNCHER_PID)

  trace.add_atrace_begin(
      ts=127_000_000, buf="animation", tid=LAUNCHER_UI_TID, pid=LAUNCHER_PID)
  trace.add_atrace_end(ts=138_000_000, tid=LAUNCHER_UI_TID, pid=LAUNCHER_PID)

  # Add expected and actual frames.
  add_expected_surface_frame_events(
      ts=104_000_000, dur=16_000_000, token=75, pid=LAUNCHER_PID)
  add_actual_surface_frame_events(
      ts=104_000_000, dur=16_000_000, token=75, layer=LAYER_2, pid=LAUNCHER_PID)

  add_expected_surface_frame_events(
      ts=121_000_000, dur=16_000_000, token=80, pid=LAUNCHER_PID)
  add_actual_surface_frame_events(
      ts=121_000_000, dur=7_000_000, token=80, layer=LAYER_2, pid=LAUNCHER_PID)

  add_expected_surface_frame_events(
      ts=139_000_000, dur=16_000_000, token=82, pid=LAUNCHER_PID)
  add_actual_surface_frame_events(
      ts=139_000_000, dur=6_000_000, token=82, layer=LAYER_2, pid=LAUNCHER_PID)


def add_ignored_latency_cujs(trace):
  cuj_1 = "L<IGNORED_CUJ_1>"
  cuj_2 = "L<IGNORED_CUJ_2>"
  trace.add_async_atrace_for_thread(
      ts=150_000_000,
      ts_end=155_000_000,
      buf=cuj_1,
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID)
  trace.add_async_atrace_for_thread(
      ts=156_000_000,
      ts_end=160_000_000,
      buf=cuj_2,
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID)

  trace.add_atrace_instant(
      ts=150_000_001, buf=cuj_1 + "#UIThread", pid=SYSUI_PID, tid=SYSUI_UI_TID)

  trace.add_atrace_instant(
      ts=156_000_001, buf=cuj_2 + "#UIThread", pid=SYSUI_PID, tid=SYSUI_UI_TID)

  trace.add_atrace_instant_for_track(
      ts=150_000_002,
      buf="FT#beginVsync#90",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=cuj_1)

  trace.add_atrace_instant_for_track(
      ts=150_000_010,
      buf="FT#layerId#0",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=cuj_1)

  trace.add_atrace_instant_for_track(
      ts=154_000_001,
      buf="FT#endVsync#92",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=cuj_1)

  trace.add_atrace_instant_for_track(
      ts=156_000_002,
      buf="FT#beginVsync#94",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=cuj_2)

  trace.add_atrace_instant_for_track(
      ts=156_000_010,
      buf="FT#layerId#0",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=cuj_2)

  trace.add_atrace_instant_for_track(
      ts=156_000_001,
      buf="FT#endVsync#96",
      pid=SYSUI_PID,
      tid=SYSUI_UI_TID,
      track_name=cuj_2)


def add_process(trace, package_name, uid, pid):
  trace.add_package_list(ts=0, name=package_name, uid=uid, version_code=1)
  trace.add_process(pid=pid, ppid=pid, cmdline=package_name, uid=uid)
  trace.add_thread(tid=pid, tgid=pid, cmdline="MainThread", name="MainThread")


def setup_trace():
  trace = synth_common.create_trace()
  trace.add_packet()
  add_process(trace, package_name=SYSUI_PACKAGE, uid=SYSUI_UID, pid=SYSUI_PID)
  add_process(
      trace, package_name=LAUNCHER_PACKAGE, uid=LAUNCHER_UID, pid=LAUNCHER_PID)

  trace.add_thread(
      tid=SYSUI_UI_TID,
      tgid=SYSUI_PID,
      cmdline="BackPanelUiThre",
      name="BackPanelUiThre")

  trace.add_ftrace_packet(cpu=0)
  return trace


trace = setup_trace()
add_blocking_calls_per_frame_multiple_cuj_instance(trace, FIRST_CUJ)
trace.add_ftrace_packet(cpu=0)
add_blocking_call_crossing_frame_boundary(trace, SECOND_CUJ)
trace.add_ftrace_packet(cpu=0)
add_ignored_latency_cujs(trace)
sys.stdout.buffer.write(trace.trace.SerializeToString())
