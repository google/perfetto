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
#
# Synthetic trace modelling how Chrome on Android reports a web-content scroll
# CUJ:
#
#  * The `J<WebContentsScroll>` slice is a TrackEvent on a process-scoped named
#    track in the *renderer* process. Vsync bounds and completion state are
#    carried as typed `ChromeTrackEvent.android_cuj_metrics` arguments (BEGIN
#    carries begin_vsync_id, END carries end_vsync_id + canceled) instead of the
#    dynamically named `FT#beginVsync#<id>` instants used by Android apps.
#  * There are no `Choreographer#doFrame` slices: Chrome composites in the GPU
#    process, which owns the SurfaceFlinger layer
#    (`TX - com.android.chrome/ChromeChildSurface#308`).
#  * Other layers present in the same vsync range (the browser's activity window
#    at a single vsync and SystemUI) and must not be attributed to the CUJ.
#
# Scroll 1: vsyncs 100..104, completed.
# Scroll 2: vsyncs 200..201, canceled (torn down mid scroll) -> excluded.

import synth_common
from os import sys

RENDERER_PID = 2001
RENDERER_COMPOSITOR_TID = 2010
GPU_PID = 2002
GPU_MAIN_TID = 2020
BROWSER_PID = 2000
BROWSER_MAIN_TID = 2000
SYSUI_PID = 3000
SYSUI_MAIN_TID = 3000

RENDERER_PROCESS_TRACK = 100
CUJ_TRACK_SCROLL_1 = 101
CUJ_TRACK_SCROLL_2 = 102

CHROME_LAYER = "TX - com.android.chrome/ChromeChildSurface#308"
BROWSER_LAYER = "TX - com.android.chrome/com.google.android.apps.chrome.Main#302"
SYSUI_LAYER = "TX - StatusBar#92"

VSYNC_NS = 8_000_000


def add_surface_frame(trace, vsync, pid, layer, on_time=True):
  # Frame work starts one vsync before presentation. Cookies must be unique
  # per (layer, vsync) across the trace.
  cookie_base = (vsync * 100 + pid % 100) * 10
  ts = vsync * VSYNC_NS
  trace.add_expected_surface_frame_start_event(
      ts=ts,
      cookie=cookie_base + 1,
      token=vsync,
      display_frame_token=vsync + 1000,
      pid=pid,
      layer_name=layer)
  trace.add_frame_end_event(ts=ts + VSYNC_NS, cookie=cookie_base + 1)
  trace.add_actual_surface_frame_start_event(
      ts=ts,
      cookie=cookie_base + 2,
      token=vsync,
      display_frame_token=vsync + 1000,
      pid=pid,
      layer_name=layer,
      present_type=1,  # PRESENT_ON_TIME
      on_time_finish=1 if on_time else 0,
      gpu_composition=0,
      jank_type=1 if on_time else 64,  # JANK_NONE / JANK_APP_DEADLINE_MISSED
      prediction_type=1)  # PREDICTION_VALID
  trace.add_frame_end_event(
      ts=ts + (VSYNC_NS if on_time else 2 * VSYNC_NS), cookie=cookie_base + 2)


def add_cuj(trace, track, begin_vsync, end_vsync, canceled):
  cuj_metrics = trace.prototypes.ChromeTrackEvent.DESCRIPTOR.extensions_by_name[
      'android_cuj_metrics']

  # BEGIN at the first damaging frame's begin-frame time.
  packet = trace.add_track_event_slice_begin(
      "J<WebContentsScroll>", ts=begin_vsync * VSYNC_NS, track=track)
  packet.track_event.categories.append("input.scrolling")
  packet.track_event.Extensions[cuj_metrics].begin_vsync_id = begin_vsync

  # END at the last damaging frame's presentation.
  packet = trace.add_track_event_slice_end(
      ts=end_vsync * VSYNC_NS + VSYNC_NS, track=track)
  packet.track_event.categories.append("input.scrolling")
  packet.track_event.Extensions[cuj_metrics].end_vsync_id = end_vsync
  packet.track_event.Extensions[cuj_metrics].canceled = canceled


trace = synth_common.create_trace()

trace.add_packet()
trace.add_process(pid=BROWSER_PID, ppid=1, cmdline="com.android.chrome")
trace.add_process(
    pid=RENDERER_PID,
    ppid=1,
    cmdline="com.android.chrome:sandboxed_process0:"
    "org.chromium.content.app.SandboxedProcessService0:0")
trace.add_process(
    pid=GPU_PID, ppid=1, cmdline="com.android.chrome:privileged_process0")
trace.add_process(pid=SYSUI_PID, ppid=1, cmdline="com.android.systemui")
trace.add_thread(
    tid=BROWSER_MAIN_TID,
    tgid=BROWSER_PID,
    cmdline="CrBrowserMain",
    name="CrBrowserMain")
trace.add_thread(
    tid=RENDERER_COMPOSITOR_TID,
    tgid=RENDERER_PID,
    cmdline="Compositor",
    name="Compositor")
trace.add_thread(
    tid=GPU_MAIN_TID,
    tgid=GPU_PID,
    cmdline="VizCompositorTh",
    name="VizCompositorTh")
trace.add_thread(
    tid=SYSUI_MAIN_TID,
    tgid=SYSUI_PID,
    cmdline="com.android.systemui",
    name="ndroid.systemui")

# Renderer process track and one named child track per scroll (Chrome keys
# the NamedTrack by the first damaging frame's result id).
trace.add_process_track_descriptor(RENDERER_PROCESS_TRACK, pid=RENDERER_PID)
for track in (CUJ_TRACK_SCROLL_1, CUJ_TRACK_SCROLL_2):
  packet = trace.add_track_descriptor(track, parent=RENDERER_PROCESS_TRACK)
  packet.track_descriptor.static_name = "J<WebContentsScroll>"

# Scroll 1: completed, vsyncs 100..104 presented by the GPU process' web
# content layer. Frame 102 misses the app deadline.
add_cuj(
    trace, CUJ_TRACK_SCROLL_1, begin_vsync=100, end_vsync=104, canceled=False)
for vsync in range(100, 105):
  add_surface_frame(trace, vsync, GPU_PID, CHROME_LAYER, on_time=(vsync != 102))
# The browser's own window presents once in the range (e.g. toolbar) ...
add_surface_frame(trace, 101, BROWSER_PID, BROWSER_LAYER)
# ... and SystemUI presents concurrently. Neither belongs to the CUJ.
add_surface_frame(trace, 102, SYSUI_PID, SYSUI_LAYER)

# Scroll 2: canceled (renderer torn down mid scroll).
add_cuj(
    trace, CUJ_TRACK_SCROLL_2, begin_vsync=200, end_vsync=201, canceled=True)
for vsync in range(200, 202):
  add_surface_frame(trace, vsync, GPU_PID, CHROME_LAYER)

sys.stdout.buffer.write(trace.trace.SerializeToString())
