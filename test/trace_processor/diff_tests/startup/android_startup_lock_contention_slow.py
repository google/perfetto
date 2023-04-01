#!/usr/bin/env python3
# Copyright (C) 2022 The Android Open Source Project
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

from os import sys

import synth_common


def to_s(ts):
  return ts * 1000 * 1000 * 1000


trace = synth_common.create_trace()
trace.add_packet()
trace.add_process(1, 0, 'init')
trace.add_process(2, 1, 'system_server')
trace.add_process(3, 1, 'com.google.android.calendar', 10001)
trace.add_thread(4, 3, 'Binder')

trace.add_package_list(
    ts=to_s(1), name='com.google.android.calendar', uid=10001, version_code=123)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_async_begin(
    ts=to_s(110), tid=2, pid=2, buf='launchingActivity#1')
trace.add_atrace_async_end(
    ts=to_s(210), tid=2, pid=2, buf='launchingActivity#1')

# Required so we know this process is the one being started up.
trace.add_atrace_begin(ts=to_s(112), tid=3, pid=3, buf='bindApplication')
trace.add_atrace_end(ts=to_s(115), tid=3, pid=3)
trace.add_atrace_begin(ts=to_s(115), tid=3, pid=3, buf='activityStart')
trace.add_atrace_end(ts=to_s(116), tid=3, pid=3)
trace.add_atrace_begin(ts=to_s(116), tid=3, pid=3, buf='activityResume')
trace.add_atrace_end(ts=to_s(117), tid=3, pid=3)

# Add some non-monitor lock contention.
trace.add_atrace_begin(
    ts=to_s(120),
    tid=3,
    pid=3,
    buf='Lock contention on thread list lock (owner tid: 2)')
trace.add_atrace_end(ts=to_s(130), tid=3, pid=3)

# Add monitor contention
trace.add_atrace_begin(
    ts=to_s(140),
    tid=3,
    pid=3,
    buf='Lock contention on a monitor lock (owner tid: 2)')
trace.add_atrace_end(ts=to_s(157), tid=3, pid=3)

# Lock contention on non-main thread should not be counted.
trace.add_atrace_begin(
    ts=to_s(155),
    tid=4,
    pid=3,
    buf='Lock contention on a monitor lock (owner tid: 3)')
trace.add_atrace_end(ts=to_s(160), tid=4, pid=3)

# Lock contention in other process should not be counted.
trace.add_atrace_begin(
    ts=to_s(175),
    tid=2,
    pid=2,
    buf='Lock contention on a monitor lock (owner tid: 3)')
trace.add_atrace_end(ts=to_s(180), tid=2, pid=2)

trace.add_atrace_instant(
    ts=to_s(211),
    tid=2,
    pid=2,
    buf='launchingActivity#1:completed:com.google.android.calendar')

sys.stdout.buffer.write(trace.trace.SerializeToString())
