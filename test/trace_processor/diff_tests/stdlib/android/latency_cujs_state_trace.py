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
import sys

SYSUI_PID = 5000
SYSUI_UI_TID = 5020


def setup_trace():
  trace = synth_common.create_trace()
  trace.add_packet()
  trace.add_package_list(
      ts=0, name="com.android.systemui", uid=10001, version_code=1)
  trace.add_process(
      pid=SYSUI_PID, ppid=SYSUI_PID, cmdline="com.android.systemui", uid=10001)
  trace.add_thread(
      tid=SYSUI_PID, tgid=SYSUI_PID, cmdline="MainThread", name="MainThread")
  trace.add_ftrace_packet(cpu=0)
  return trace


trace = setup_trace()

# CUJ 1: Completed
trace.add_async_atrace_for_thread(
    ts=10_000_000,
    ts_end=20_000_000,
    buf="L<CUJ_COMPLETED>",
    pid=SYSUI_PID,
    tid=SYSUI_UI_TID)

# CUJ 2: Canceled
trace.add_async_atrace_for_thread(
    ts=30_000_000,
    ts_end=40_000_000,
    buf="L<CUJ_CANCELED>",
    pid=SYSUI_PID,
    tid=SYSUI_UI_TID)
trace.add_atrace_instant_for_track(
    ts=35_000_000,
    buf="cancel",
    pid=SYSUI_PID,
    tid=SYSUI_UI_TID,
    track_name="L<CUJ_CANCELED>")

# CUJ 3: Timeout
trace.add_async_atrace_for_thread(
    ts=50_000_000,
    ts_end=60_000_000,
    buf="L<CUJ_TIMEOUT>",
    pid=SYSUI_PID,
    tid=SYSUI_UI_TID)
trace.add_atrace_instant_for_track(
    ts=55_000_000,
    buf="timeout",
    pid=SYSUI_PID,
    tid=SYSUI_UI_TID,
    track_name="L<CUJ_TIMEOUT>")

sys.stdout.buffer.write(trace.trace.SerializeToString())
