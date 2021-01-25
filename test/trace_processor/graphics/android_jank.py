#!/usr/bin/env python3
# Copyright (C) 2020 The Android Open Source Project
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


def add_main_thread_atrace(trace, ts, ts_end, buf):
  trace.add_atrace_begin(ts=ts, tid=PID, pid=PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=PID, pid=PID)


def add_render_thread_atrace(trace, ts, ts_end, buf):
  trace.add_atrace_begin(ts=ts, tid=RTID, pid=PID, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=RTID, pid=PID)


trace = synth_common.create_trace()

trace.add_packet()
trace.add_package_list(
    ts=0, name="com.android.systemui", uid=10001, version_code=1)
trace.add_package_list(
    ts=0,
    name="com.google.android.inputmethod.latin",
    uid=10002,
    version_code=1)

trace.add_process(pid=1000, ppid=1, cmdline="com.android.systemui", uid=10001)
trace.add_thread(
    tid=1001, tgid=1000, cmdline="RenderThread", name="RenderThread")
trace.add_process(
    pid=2000, ppid=1, cmdline="com.google.android.inputmethod.latin", uid=10002)
trace.add_thread(
    tid=2001, tgid=2000, cmdline="RenderThread", name="RenderThread")

trace.add_ftrace_packet(cpu=0)

# com.android.systemui

trace.add_atrace_begin(
    ts=1_000_000, tid=1000, pid=1000, buf='Choreographer#doFrame')
trace.add_atrace_begin(ts=1_000_100, tid=1000, pid=1000, buf='traversal')
trace.add_atrace_begin(ts=1_000_500, tid=1000, pid=1000, buf='measure')
trace.add_atrace_end(ts=4_000_000, tid=1000, pid=1000)
trace.add_atrace_begin(ts=4_000_500, tid=1000, pid=1000, buf='layout')
trace.add_atrace_begin(ts=4_001_000, tid=1000, pid=1000, buf='setupListItem')
trace.add_atrace_begin(ts=4_500_000, tid=1000, pid=1000, buf='inflate')
trace.add_atrace_end(ts=5_500_000, tid=1000, pid=1000)
trace.add_atrace_begin(ts=6_500_000, tid=1000, pid=1000, buf='inflate')
trace.add_atrace_end(ts=7_500_000, tid=1000, pid=1000)
trace.add_atrace_end(ts=7_500_500, tid=1000, pid=1000)
trace.add_atrace_begin(ts=8_000_000, tid=1000, pid=1000, buf='obtainView')
trace.add_atrace_begin(ts=8_000_100, tid=1000, pid=1000, buf='inflate')
trace.add_atrace_end(ts=8_500_000, tid=1000, pid=1000)
trace.add_atrace_end(ts=8_900_000, tid=1000, pid=1000)
trace.add_atrace_end(ts=9_000_000, tid=1000, pid=1000)
trace.add_atrace_end(ts=9_000_000, tid=1000, pid=1000)
trace.add_atrace_end(ts=20_000_000, tid=1000, pid=1000)

trace.add_sched(ts=1_000_000, prev_pid=0, next_pid=1000)
trace.add_sched(ts=10_000_000, prev_pid=1000, next_pid=0, prev_state='R')
trace.add_sched(ts=10_500_000, prev_pid=0, next_pid=0)
trace.add_sched(ts=19_500_000, prev_pid=0, next_pid=1000)
trace.add_sched(ts=20_500_000, prev_pid=1000, next_pid=0, prev_state='R')

# com.google.android.inputmethod.latin

trace.add_atrace_begin(
    ts=101_000_000, tid=2000, pid=2000, buf='Choreographer#doFrame')
trace.add_atrace_begin(ts=101_000_100, tid=2000, pid=2000, buf='traversal')
trace.add_atrace_begin(ts=101_000_500, tid=2000, pid=2000, buf='measure')
trace.add_atrace_end(ts=104_000_000, tid=2000, pid=2000)
trace.add_atrace_begin(ts=104_000_500, tid=2000, pid=2000, buf='layout')
trace.add_atrace_end(ts=105_000_000, tid=2000, pid=2000)
trace.add_atrace_end(ts=105_000_000, tid=2000, pid=2000)
trace.add_atrace_begin(ts=105_000_000, tid=2000, pid=2000, buf='draw')
trace.add_atrace_end(ts=119_000_000, tid=2000, pid=2000)
trace.add_atrace_end(ts=120_000_000, tid=2000, pid=2000)

trace.add_atrace_begin(ts=105_000_000, tid=2001, pid=2000, buf='DrawFrame')
trace.add_atrace_begin(
    ts=108_000_000, tid=2001, pid=2000, buf='Upload 300x300 Texture')
trace.add_atrace_end(ts=112_000_000, tid=2001, pid=2000)
trace.add_atrace_begin(
    ts=116_000_000,
    tid=2001,
    pid=2000,
    buf='alpha caused unclipped saveLayer 201x319')
trace.add_atrace_end(ts=117_300_000, tid=2001, pid=2000)
trace.add_atrace_end(ts=118_000_000, tid=2001, pid=2000)

trace.add_sched(ts=101_000_000, prev_pid=0, next_pid=2000)
trace.add_sched(ts=120_000_000, prev_pid=2000, next_pid=0, prev_state='R')
trace.add_sched(ts=120_500_000, prev_pid=0, next_pid=0)

sys.stdout.buffer.write(trace.trace.SerializeToString())
