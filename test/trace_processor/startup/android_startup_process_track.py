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


def add_startup(trace, ts, pid):
  trace.add_ftrace_packet(cpu=0)
  trace.add_atrace_begin(
      ts=ts,
      tid=2,
      pid=2,
      buf='MetricsLogger:launchObserverNotifyIntentStarted')
  trace.add_atrace_end(ts=ts + 1, tid=2, pid=2)
  trace.add_atrace_async_begin(
      ts=ts + 2, tid=2, pid=2, buf='launching: com.google.android.calendar')
  trace.add_newtask(
      ts=ts + 3,
      tid=1,
      new_tid=pid,
      new_comm='com.google.android.calendar',
      flags=0)
  trace.add_atrace_begin(ts=ts + 3, tid=pid, pid=pid, buf='bindApplication')
  trace.add_atrace_end(ts=ts + 4, tid=pid, pid=pid)
  trace.add_atrace_begin(ts=ts + 4, tid=pid, pid=pid, buf='activityStart')
  trace.add_atrace_end(ts=ts + 5, tid=pid, pid=pid)
  trace.add_atrace_begin(ts=ts + 5, tid=pid, pid=pid, buf='activityResume')
  trace.add_atrace_end(ts=ts + 6, tid=pid, pid=pid)
  trace.add_atrace_async_end(
      ts=ts + 7, tid=2, pid=2, buf='launching: com.google.android.calendar')
  trace.add_atrace_begin(
      ts=ts + 7,
      tid=2,
      pid=2,
      buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
  trace.add_atrace_end(ts=ts + 8, tid=2, pid=2)


# Build a trace where calendar starts, exits and restarts.
# Verify that each startup is only associated with a single process
# (i.e. process exit is taken into account).
trace = synth_common.create_trace()
trace.add_packet()
trace.add_process(1, 0, 'init', uid=10001)
trace.add_process(2, 1, 'system_server', uid=1000)

trace.add_package_list(
    ts=99, name='com.google.android.calendar', uid=10001, version_code=123)

add_startup(trace, ts=100, pid=3)
trace.add_packet(ts=140)
trace.add_process(3, 1, 'com.google.android.calendar:debug', uid=10001)

trace.add_packet()
trace.add_process_free(ts=150, tid=3, comm='', prio=0)

add_startup(trace, ts=200, pid=4)
trace.add_packet(ts=250)
trace.add_process(4, 1, 'com.google.android.calendar', uid=10001)

sys.stdout.buffer.write(trace.trace.SerializeToString())
