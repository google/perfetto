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

trace = synth_common.create_trace()
trace.add_packet()
trace.add_process(1, 0, 'init')
trace.add_process(2, 1, 'system_server')
trace.add_process(3, 1, 'com.google.android.calendar', 10001)

trace.add_debuggable_package_list(
    ts=1, name='com.google.android.calendar', uid=10001, version_code=123)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_async_begin(ts=110, tid=2, pid=2, buf='launchingActivity#1')
trace.add_atrace_async_end(ts=210, tid=2, pid=2, buf='launchingActivity#1')
trace.add_atrace_instant(
    ts=211,
    tid=2,
    pid=2,
    buf='launchingActivity#1:completed:com.google.android.calendar')

trace.add_atrace_async_begin(ts=220, tid=2, pid=2, buf='launchingActivity#2')
trace.add_atrace_async_end(ts=230, tid=2, pid=2, buf='launchingActivity#2')
trace.add_atrace_instant(
    ts=231,
    tid=2,
    pid=2,
    buf='launchingActivity#2:completed-hot:com.google.android.calendar')
# Emulate a hot start (and therefore that we only see activityResume).
trace.add_atrace_begin(ts=221, tid=3, pid=3, buf='activityResume')
trace.add_atrace_end(ts=225, tid=3, pid=3)

sys.stdout.buffer.write(trace.trace.SerializeToString())
