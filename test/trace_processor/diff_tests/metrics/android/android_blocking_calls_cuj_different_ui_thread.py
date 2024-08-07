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

CUJ_PID = 5000
CUJ_MAIN_TID = 5000
CUJ_UI_TID = 5020
BACK_PANEL_UI_THREAD_TID = 5040

PROCESS_TRACK = 1234
CUJ_ASYNC_TRACK = 535
CUJ_HARDCODED_UI_THREAD_ASYNC_TRACK = 536
LAYER = "TX - some_layer#0"


def add_ui_thread_atrace(trace, ts, ts_end, buf, tid):
  trace.add_atrace_begin(ts=ts, tid=tid, pid=CUJ_PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=tid, pid=CUJ_PID)


def add_instant_event_in_thread(trace, ts, buf, pid, tid):
  trace.add_atrace_instant(ts=ts, tid=tid, pid=pid, buf=buf)


def add_frame(trace, vsync, ts_do_frame, ts_end_do_frame, tid):
  add_ui_thread_atrace(
      trace,
      ts=ts_do_frame,
      ts_end=ts_end_do_frame,
      buf="Choreographer#doFrame %d" % vsync,
      tid=tid)


def add_expected_surface_frame_events(ts, dur, token):
  trace.add_expected_surface_frame_start_event(
      ts=ts,
      cookie=100000 + token,
      token=token,
      display_frame_token=100 + token,
      pid=CUJ_PID,
      layer_name='')
  trace.add_frame_end_event(ts=ts + dur, cookie=100000 + token)


def add_actual_surface_frame_events(ts, dur, token):
  cookie = token + 1
  trace.add_actual_surface_frame_start_event(
      ts=ts,
      cookie=100002 + cookie,
      token=token,
      display_frame_token=token + 100,
      pid=CUJ_PID,
      present_type=1,
      on_time_finish=1,
      gpu_composition=0,
      jank_type=1,
      prediction_type=3,
      layer_name=LAYER)
  trace.add_frame_end_event(ts=ts + dur, cookie=100002 + cookie)


def setup_trace():
  trace = synth_common.create_trace()
  trace.add_packet()
  package_name = "com.google.android.with.custom.ui.thread"
  uid = 10001
  trace.add_package_list(ts=0, name=package_name, uid=uid, version_code=1)
  trace.add_process(pid=CUJ_PID, ppid=1, cmdline=package_name, uid=uid)
  trace.add_thread(
      tid=CUJ_MAIN_TID, tgid=CUJ_PID, cmdline="MainThread", name="MainThread")
  trace.add_thread(
      tid=CUJ_UI_TID, tgid=CUJ_PID, cmdline="UIThread", name="UIThread")
  trace.add_thread(
      tid=BACK_PANEL_UI_THREAD_TID,
      tgid=CUJ_PID,
      cmdline="BackPanelUiThre",
      name="BackPanelUiThre")
  trace.add_process_track_descriptor(PROCESS_TRACK, pid=CUJ_PID)
  trace.add_track_descriptor(CUJ_ASYNC_TRACK, parent=PROCESS_TRACK)
  trace.add_track_descriptor(
      CUJ_HARDCODED_UI_THREAD_ASYNC_TRACK, parent=PROCESS_TRACK)
  trace.add_ftrace_packet(cpu=0)
  return trace


def setup_cujs(trace):
  # CUJ to test the new behaviour, where the ui thread is get from an instant
  # event.
  cuj_name = "J<CUJ_NAME>"
  # Tests the deprecated behaviour where the ui thread is get from an hardcoded
  # list.
  back_panel_cuj = "J<BACK_PANEL_ARROW>"

  # UI Thread marker, soon after cuj start
  add_instant_event_in_thread(
      trace,
      ts=5_000_001,
      buf=cuj_name + "#UIThread",
      pid=CUJ_PID,
      tid=CUJ_UI_TID)

  # The following is a blocking call during theh cuj, on the cuj ui thread.
  # It is expected to appear in the output
  trace.add_atrace_begin(
      ts=14_000_000, buf="measure", tid=CUJ_UI_TID, pid=CUJ_PID)
  trace.add_atrace_end(ts=16_000_000, tid=CUJ_UI_TID, pid=CUJ_PID)
  # The following is not expected in the output as it's in another thread!
  trace.add_atrace_begin(
      ts=14_000_000, buf="layout", tid=CUJ_MAIN_TID, pid=CUJ_PID)
  trace.add_atrace_end(ts=16_000_000, tid=CUJ_MAIN_TID, pid=CUJ_PID)

  trace.add_atrace_begin(
      ts=15_000_000, buf="animation", tid=BACK_PANEL_UI_THREAD_TID, pid=CUJ_PID)
  trace.add_atrace_end(ts=17_000_000, tid=BACK_PANEL_UI_THREAD_TID, pid=CUJ_PID)

  add_frame(
      trace,
      vsync=10,
      ts_do_frame=10_000_000,
      ts_end_do_frame=18_000_000,
      tid=CUJ_UI_TID)

  add_frame(
      trace,
      vsync=20,
      ts_do_frame=10_000_000,
      ts_end_do_frame=18_000_000,
      tid=BACK_PANEL_UI_THREAD_TID)

  trace.add_track_event_slice_begin(
      ts=5_000_000, track=CUJ_ASYNC_TRACK, name=cuj_name)
  trace.add_track_event_slice_end(ts=20_000_000, track=CUJ_ASYNC_TRACK)

  trace.add_track_event_slice_begin(
      ts=5_000_000,
      track=CUJ_HARDCODED_UI_THREAD_ASYNC_TRACK,
      name=back_panel_cuj)
  trace.add_track_event_slice_end(
      ts=20_000_000, track=CUJ_HARDCODED_UI_THREAD_ASYNC_TRACK)

  add_expected_surface_frame_events(ts=0, dur=16_000_000, token=10)
  add_actual_surface_frame_events(ts=0, dur=16_000_000, token=10)

  add_expected_surface_frame_events(ts=0, dur=16_000_000, token=20)
  add_actual_surface_frame_events(ts=0, dur=16_000_000, token=20)


trace = setup_trace()
setup_cujs(trace)
sys.stdout.buffer.write(trace.trace.SerializeToString())
