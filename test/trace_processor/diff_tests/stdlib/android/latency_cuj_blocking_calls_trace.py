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

# A latency CUJ with blocking calls on the main thread, the RenderThread and a
# background thread of SystemUI. Another process with some threads is added
# first, so that the upid of SystemUI differs from the utid of its main thread.

import synth_common
import sys

OTHER_PID = 100
OTHER_TIDS = [101, 102, 103]
SYSUI_PID = 1000
SYSUI_RTID = 1500
SYSUI_BG_TID = 1600

trace = synth_common.create_trace()

trace.add_packet()
trace.add_process(pid=OTHER_PID, ppid=1, cmdline="com.android.other", uid=10002)
for tid in OTHER_TIDS:
  trace.add_thread(tid=tid, tgid=OTHER_PID, cmdline="Worker", name="Worker")

trace.add_packet()
trace.add_package_list(
    ts=0, name="com.android.systemui", uid=10001, version_code=1)
trace.add_process(
    pid=SYSUI_PID, ppid=1, cmdline="com.android.systemui", uid=10001)
trace.add_thread(
    tid=SYSUI_RTID, tgid=SYSUI_PID, cmdline="RenderThread", name="RenderThread")
trace.add_thread(
    tid=SYSUI_BG_TID, tgid=SYSUI_PID, cmdline="Background", name="Background")

trace.add_ftrace_packet(cpu=0)

trace.add_async_atrace_for_thread(
    ts=10_000_000,
    ts_end=30_000_000,
    buf="L<ACTION_EXPAND_PANEL>",
    tid=SYSUI_PID,
    pid=SYSUI_PID)

# Main thread: must be in android_cuj_blocking_calls.
trace.add_atrace_for_thread(
    ts=12_000_000,
    ts_end=14_000_000,
    buf="measure",
    tid=SYSUI_PID,
    pid=SYSUI_PID)
trace.add_atrace_for_thread(
    ts=15_000_000,
    ts_end=18_000_000,
    buf="layout",
    tid=SYSUI_PID,
    pid=SYSUI_PID)

# RenderThread: must be in android_cuj_blocking_calls.
trace.add_atrace_for_thread(
    ts=19_000_000,
    ts_end=20_000_000,
    buf="CreateGraphicsPipeline",
    tid=SYSUI_RTID,
    pid=SYSUI_PID)

# Background thread: must not be in android_cuj_blocking_calls.
trace.add_atrace_for_thread(
    ts=21_000_000,
    ts_end=22_000_000,
    buf="inflate",
    tid=SYSUI_BG_TID,
    pid=SYSUI_PID)

sys.stdout.buffer.write(trace.trace.SerializeToString())
