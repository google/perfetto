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

# A jank CUJ whose FT#beginVsync is stale: the CUJ thread was idle before the
# CUJ started, so the begin vsync reported by FrameTracker belongs to a frame
# drawn ~1s before the CUJ. Frames rendered before the CUJ started must not be
# attributed to the CUJ. Neither must a frame within the vsync range that only
# started after the CUJ ended.

from os import sys

import synth_common

# com.android.systemui
PID = 1000
LAYER = "TX - Task=1#0"

PROCESS_TRACK = 1234
CUJ_TRACK = 4321
CUJ = "J<STALE_BEGIN_VSYNC>"

# The CUJ slice starts in the first frame of the CUJ (vsync 100).
CUJ_TS = 1_000_000_000
CUJ_TS_END = 1_050_000_000


def add_main_thread_atrace(trace, ts, ts_end, buf):
  trace.add_atrace_begin(ts=ts, tid=PID, pid=PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=PID, pid=PID)


def add_do_frame(trace, vsync, ts, ts_end):
  add_main_thread_atrace(trace, ts, ts_end, "Choreographer#doFrame %d" % vsync)


def add_expected_surface_frame_events(trace, ts, dur, token):
  trace.add_expected_surface_frame_start_event(
      ts=ts,
      cookie=100000 + token,
      token=token,
      display_frame_token=100 + token,
      pid=PID,
      layer_name='')
  trace.add_frame_end_event(ts=ts + dur, cookie=100000 + token)


def add_actual_surface_frame_events(trace, ts, dur, token, jank=None):
  jank_type = jank if jank is not None else 1
  present_type = 2 if jank is not None else 1
  on_time_finish = 1 if jank is None else 0
  trace.add_actual_surface_frame_start_event(
      ts=ts,
      cookie=200000 + token,
      token=token,
      display_frame_token=100 + token,
      pid=PID,
      present_type=present_type,
      on_time_finish=on_time_finish,
      gpu_composition=0,
      jank_type=jank_type,
      prediction_type=3,
      layer_name=LAYER,
      jank_score=0 if jank is None else 1)
  trace.add_frame_end_event(ts=ts + dur, cookie=200000 + token)


trace = synth_common.create_trace()

trace.add_packet()
trace.add_package_list(
    ts=0, name="com.android.systemui", uid=10001, version_code=1)
trace.add_process(pid=PID, ppid=1, cmdline="com.android.systemui", uid=10001)

trace.add_process_track_descriptor(PROCESS_TRACK, pid=PID)
trace.add_track_descriptor(CUJ_TRACK, parent=PROCESS_TRACK)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_instant(
    ts=CUJ_TS + 1_000, buf=CUJ + "#UIThread", pid=PID, tid=PID)

# The CUJ slice and its markers. FT#beginVsync#11 is stale: it points right
# after the last frame (vsync 10) drawn by the CUJ thread long before the CUJ.
trace.add_track_event_slice_begin(ts=CUJ_TS, track=CUJ_TRACK, name=CUJ)
trace.add_track_event_slice(
    ts=CUJ_TS + 2_000, dur=0, track=CUJ_TRACK, name="FT#beginVsync#11")
trace.add_track_event_slice(
    ts=CUJ_TS_END - 2_000, dur=0, track=CUJ_TRACK, name="FT#end#0")
trace.add_track_event_slice(
    ts=CUJ_TS_END - 1_000, dur=0, track=CUJ_TRACK, name="FT#endVsync#110")
trace.add_track_event_slice_end(ts=CUJ_TS_END, track=CUJ_TRACK)

trace.add_ftrace_packet(cpu=0)

# Frames of an earlier animation, ~1s before the CUJ. Frame 20 is janky.
add_do_frame(trace, vsync=10, ts=0, ts_end=5_000_000)
add_do_frame(trace, vsync=20, ts=16_000_000, ts_end=21_000_000)

# Frames of the CUJ. The first doFrame starts just before the CUJ slice (the
# CUJ slice is started from a callback of this frame).
add_do_frame(trace, vsync=100, ts=CUJ_TS - 10_000, ts_end=CUJ_TS + 5_000_000)
add_do_frame(
    trace, vsync=110, ts=CUJ_TS + 16_000_000, ts_end=CUJ_TS + 21_000_000)

add_expected_surface_frame_events(trace, ts=0, dur=16_000_000, token=10)
add_actual_surface_frame_events(trace, ts=0, dur=16_000_000, token=10)

add_expected_surface_frame_events(
    trace, ts=16_000_000, dur=16_000_000, token=20)
add_actual_surface_frame_events(
    trace, ts=16_000_000, dur=30_000_000, token=20, jank=64)

add_expected_surface_frame_events(
    trace, ts=CUJ_TS - 10_000, dur=16_000_000, token=100)
add_actual_surface_frame_events(
    trace, ts=CUJ_TS - 10_000, dur=16_000_000, token=100)

add_expected_surface_frame_events(
    trace, ts=CUJ_TS + 16_000_000, dur=16_000_000, token=110)
add_actual_surface_frame_events(
    trace, ts=CUJ_TS + 16_000_000, dur=16_000_000, token=110)

# A janky frame whose vsync (105) is within [FT#beginVsync, FT#endVsync] but
# which started ~1s after the CUJ ended (out-of-order vsync tokens can happen
# e.g. for other surfaces). It must not be attributed to the CUJ either.
add_expected_surface_frame_events(
    trace, ts=CUJ_TS_END + 1_000_000_000, dur=16_000_000, token=105)
add_actual_surface_frame_events(
    trace, ts=CUJ_TS_END + 1_000_000_000, dur=30_000_000, token=105, jank=64)

sys.stdout.buffer.write(trace.trace.SerializeToString())
