#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

def add_main_thread_atrace(trace, ts, ts_end, buf, pid):
  trace.add_atrace_begin(ts=ts, tid=pid, pid=pid, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=pid, pid=pid)


def add_async_trace(trace, ts, ts_end, buf, pid):
  trace.add_atrace_async_begin(ts=ts, tid=pid, pid=pid, buf=buf)
  trace.add_atrace_async_end(ts=ts_end, tid=pid, pid=pid, buf=buf)

def add_render_thread_atrace_begin(trace, ts, buf, rtid, pid):
  trace.add_atrace_begin(ts=ts, tid=rtid, pid=pid, buf=buf)


def add_render_thread_atrace_end(trace, ts_end, rtid, pid):
  trace.add_atrace_end(ts=ts_end, tid=rtid, pid=pid)

def add_ui_thread_atrace(trace, ts, ts_end, buf, tid, pid):
  trace.add_atrace_begin(ts=ts, tid=tid, pid=pid, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=tid, pid=pid)

def add_frame(trace, vsync, ts_do_frame, ts_end_do_frame, tid, pid):
  add_ui_thread_atrace(
      trace,
      ts=ts_do_frame,
      ts_end=ts_end_do_frame,
      buf="Choreographer#doFrame %d" % vsync,
      tid=tid,
      pid=pid)

def add_expected_surface_frame_events(trace, ts, dur, token, pid):
  trace.add_expected_surface_frame_start_event(
      ts=ts,
      cookie=100000 + token,
      token=token,
      display_frame_token=100 + token,
      pid=pid,
      layer_name='')
  trace.add_frame_end_event(ts=ts + dur, cookie=100000 + token)

def add_actual_surface_frame_events(trace, ts, dur, token, layer, pid):
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

def add_instant_event_in_thread(trace, ts, buf, pid, tid):
  trace.add_atrace_instant(ts=ts, tid=tid, pid=pid, buf=buf)
