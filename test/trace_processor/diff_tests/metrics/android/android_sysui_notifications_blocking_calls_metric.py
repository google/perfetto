#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

# com.android.systemui
SYSUI_PID = 1000

THIRD_PROCESS_PID = 3000

# List of blocking calls
blocking_call_names = [
    'NotificationStackScrollLayout#onMeasure',
    'ExpNotRow#onMeasure(MessagingStyle)', 'ExpNotRow#onMeasure(BigTextStyle)',
    'NotificationShadeWindowView#onMeasure', 'ImageFloatingTextView#onMeasure',
    'Should not be in the metric'
]


def add_main_thread_atrace(trace, ts, ts_end, buf, pid):
  trace.add_atrace_begin(ts=ts, tid=pid, pid=pid, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=pid, pid=pid)


def add_async_trace(trace, ts, ts_end, buf, pid):
  trace.add_atrace_async_begin(ts=ts, tid=pid, pid=pid, buf=buf)
  trace.add_atrace_async_end(ts=ts_end, tid=pid, pid=pid, buf=buf)


# Creates a trace that contains one of each blocking call.
def add_all_sysui_notifications_blocking_calls(trace, pid):
  blocking_call_dur = 10_000_000
  blocking_call_ts = 2_000_000

  cuj_dur = len(blocking_call_names) * blocking_call_dur
  add_async_trace(
      trace,
      ts=blocking_call_ts,
      ts_end=blocking_call_ts + cuj_dur,
      buf="L<TEST_WITH_MANY_BLOCKING_CALLS>",
      pid=pid)

  for blocking_call in blocking_call_names:
    add_main_thread_atrace(
        trace,
        ts=blocking_call_ts,
        ts_end=blocking_call_ts + blocking_call_dur,
        buf=blocking_call,
        pid=pid)
    blocking_call_ts += blocking_call_dur


def add_process(trace, package_name, uid, pid):
  trace.add_package_list(ts=0, name=package_name, uid=uid, version_code=1)
  trace.add_process(pid=pid, ppid=0, cmdline=package_name, uid=uid)
  trace.add_thread(tid=pid, tgid=pid, cmdline="MainThread", name="MainThread")


def setup_trace():
  trace = synth_common.create_trace()
  trace.add_packet()
  add_process(
      trace, package_name="com.android.systemui", uid=10001, pid=SYSUI_PID)
  trace.add_ftrace_packet(cpu=0)
  return trace


trace = setup_trace()

add_all_sysui_notifications_blocking_calls(trace, pid=SYSUI_PID)

# See test_android_sysui_notifications_blocking_calls.
sys.stdout.buffer.write(trace.trace.SerializeToString())
