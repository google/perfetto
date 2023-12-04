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
trace.add_process(3, 1, 'com.google.android.calendar', uid=10001)

trace.add_package_list(
    ts=100, name='com.google.android.calendar', uid=10001, version_code=123)

trace.add_ftrace_packet(cpu=0)

# Start intent for a successful launch of calendar
trace.add_atrace_begin(
    ts=to_s(102),
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=to_s(103), tid=2, pid=2)

trace.add_atrace_async_begin(
    ts=to_s(110), tid=2, pid=2, buf='launching: com.google.android.calendar')

trace.add_atrace_begin(
    ts=to_s(120), tid=2, pid=2, buf='Start proc: com.google.android.calendar')
trace.add_atrace_end(ts=to_s(155), tid=2, pid=2)

# Unrelated process binding, ignored
trace.add_atrace_begin(ts=to_s(125), tid=1, pid=1, buf='bindApplication')
trace.add_atrace_end(ts=to_s(195), tid=1, pid=1)

trace.add_atrace_begin(ts=to_s(185), tid=3, pid=3, buf='bindApplication')
trace.add_atrace_begin(
    ts=to_s(188),
    tid=3,
    pid=3,
    buf='performCreate:com.google.android.calendar.MainActivity')
trace.add_atrace_begin(ts=to_s(188), tid=3, pid=3, buf='inflate')
trace.add_atrace_end(ts=to_s(189), tid=3, pid=3)
trace.add_atrace_begin(
    ts=to_s(188), tid=3, pid=3, buf='ResourcesManager#getResources')
trace.add_atrace_end(ts=to_s(189), tid=3, pid=3)
trace.add_atrace_begin(ts=to_s(191), tid=3, pid=3, buf='inflate')
trace.add_atrace_end(ts=to_s(192), tid=3, pid=3)
trace.add_atrace_end(ts=to_s(192), tid=3, pid=3)
trace.add_atrace_begin(
    ts=to_s(193),
    tid=3,
    pid=3,
    buf='performResume:com.google.android.calendar.MainActivity')
trace.add_atrace_end(ts=to_s(187), tid=3, pid=3)
trace.add_atrace_end(ts=to_s(195), tid=3, pid=3)

trace.add_atrace_begin(ts=to_s(195), tid=3, pid=3, buf='activityStart')
trace.add_atrace_end(ts=to_s(196), tid=3, pid=3)

trace.add_atrace_begin(ts=to_s(196), tid=3, pid=3, buf='activityResume')
trace.add_atrace_end(ts=to_s(197), tid=3, pid=3)

trace.add_atrace_begin(
    ts=to_s(200),
    tid=3,
    pid=3,
    buf='location=error status=io-error-no-oat ' \
        'filter=run-from-apk reason=unknown')
trace.add_atrace_end(ts=to_s(202), tid=3, pid=3)
trace.add_atrace_begin(
    ts=to_s(204),
    tid=3,
    pid=3,
    buf='location=/system/framework/oat/arm/com.google.android.calendar' \
        '.odex status=up-to-date filter=speed reason=install-dm')
trace.add_atrace_end(ts=to_s(205), tid=3, pid=3)

trace.add_atrace_async_end(
    ts=to_s(210), tid=2, pid=2, buf='launching: com.google.android.calendar')
trace.add_atrace_begin(
    ts=to_s(211),
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(ts=to_s(212), tid=2, pid=2)

# Add the scheduling data to match the timestamps of events above but with
# some idle time inbetween to make the computation more realisitic.
trace.add_cpufreq(ts=to_s(50), freq=1000, cpu=0)
trace.add_sched(ts=to_s(100), prev_pid=0, next_pid=2)
trace.add_sched(ts=to_s(115), prev_pid=2, next_pid=0)
trace.add_sched(ts=to_s(120), prev_pid=0, next_pid=2)
trace.add_sched(ts=to_s(125), prev_pid=2, next_pid=1)
trace.add_sched(ts=to_s(150), prev_pid=1, next_pid=2)
trace.add_sched(ts=to_s(160), prev_pid=2, next_pid=1)
trace.add_sched(ts=to_s(180), prev_pid=1, next_pid=3)
trace.add_sched(ts=to_s(205), prev_pid=3, next_pid=2)
trace.add_sched(ts=to_s(220), prev_pid=2, next_pid=0)

sys.stdout.buffer.write(trace.trace.SerializeToString())
