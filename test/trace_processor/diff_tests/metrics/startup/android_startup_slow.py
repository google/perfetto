#!/usr/bin/env python3
# Copyright (C) 2018 The Android Open Source Project
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


def to_s(ts):
  return ts * 1000 * 1000 * 1000


trace = synth_common.create_trace()
trace.add_packet()
trace.add_process(1, 0, 'init')
trace.add_process(2, 1, 'system_server')
trace.add_process(3, 1, 'com.google.android.calendar', 10001)
trace.add_process(4, 3, 'com.google.android.calendar', 10001)

trace.add_package_list(
    ts=to_s(1), name='com.google.android.calendar', uid=10001, version_code=123)

trace.add_ftrace_packet(cpu=0)
# Intent without any corresponding end state, will be ignored
trace.add_atrace_begin(
    ts=to_s(100),
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=to_s(101), tid=2, pid=2)

# Start intent for a successful launch of calendar
trace.add_atrace_begin(
    ts=to_s(102),
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=to_s(103), tid=2, pid=2)

trace.add_atrace_async_begin(
    ts=to_s(110), tid=2, pid=2, buf='launching: com.google.android.calendar')

trace.add_sched(ts=to_s(110), prev_pid=0, next_pid=3)

# As the process already existed before intent started, this is a
# warm/hot start (we choose warm). Therefore, emit an activityStart
# slice.
trace.add_atrace_begin(ts=to_s(115), tid=3, pid=3, buf='activityStart')
trace.add_atrace_end(ts=to_s(117), tid=3, pid=3)
trace.add_atrace_begin(ts=to_s(117), tid=3, pid=3, buf='activityResume')
trace.add_atrace_end(ts=to_s(118), tid=3, pid=3)

# P1: 5s interruptable sleep
trace.add_sched(ts=to_s(120), prev_pid=3, next_pid=0, prev_state='S')
trace.add_sched(ts=to_s(125), prev_pid=0, next_pid=3)
# P1: 5s blocking I/O state
trace.add_sched(ts=to_s(125), prev_pid=3, next_pid=0, prev_state='D')
trace.add_sched_blocked_reason(ts=to_s(127), pid=3, io_wait=1, unblock_pid=4)
trace.add_sched(ts=to_s(130), prev_pid=0, next_pid=3)

trace.add_sched(ts=to_s(130), prev_pid=3, next_pid=4)

# Create an unrelated task
trace.add_newtask(ts=to_s(155), tid=1, new_tid=5, new_comm='', flags=0)

# P2: 30ns running
trace.add_sched(ts=to_s(160), prev_pid=4, next_pid=0, prev_state='R')
# P2: 49ns runnable
trace.add_sched(ts=to_s(209), prev_pid=0, next_pid=4)
# P2: 1ns running
trace.add_sched(ts=to_s(210), prev_pid=4, next_pid=0)

trace.add_atrace_async_end(
    ts=to_s(210), tid=2, pid=2, buf='launching: com.google.android.calendar')
trace.add_atrace_begin(
    ts=to_s(211),
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(ts=to_s(212), tid=2, pid=2)

# Some time after, add a slice for fully drawn frame.
trace.add_atrace_begin(
    ts=to_s(300),
    tid=3,
    pid=3,
    buf='reportFullyDrawn() for \{com.google.android.calendar\}')
trace.add_atrace_end(ts=to_s(305), tid=2, pid=2)

# Start intent for calendar, we failed to launch the activity.
trace.add_atrace_begin(
    ts=to_s(402),
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=to_s(403), tid=2, pid=2)

trace.add_atrace_async_begin(
    ts=to_s(410), tid=2, pid=2, buf='launching: com.google.android.calendar')

trace.add_atrace_async_end(
    ts=to_s(510),
    tid=2,
    pid=2,
    buf='launching: com.google.android.apps.nexuslauncher')

trace.add_ftrace_packet(cpu=1)
trace.add_sched(ts=to_s(160), prev_pid=0, next_pid=1)
trace.add_sched(ts=to_s(200), prev_pid=1, next_pid=0)

sys.stdout.buffer.write(trace.trace.SerializeToString())
