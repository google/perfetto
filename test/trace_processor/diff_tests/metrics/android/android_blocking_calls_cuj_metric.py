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
# com.google.android.apps.nexuslauncher
LAUNCHER_PID = 2000

THIRD_PROCESS_PID = 3000

# List of blocking calls
blocking_call_names = [
    'monitor contention with something else', 'SuspendThreadByThreadId 123',
    'LoadApkAssetsFd 123', 'binder transaction', 'inflate',
    'Lock contention on thread list lock (owner tid: 1665)',
    'CancellableContinuationImpl#123', 'relayoutWindow*', 'measure', 'layout',
    'configChanged', 'Contending for pthread mutex',
    'ImageDecoder#decodeBitmap', 'ImageDecoder#decodeDrawable',
    'NotificationStackScrollLayout#onMeasure',
    'ExpNotRow#onMeasure(MessagingStyle)', 'ExpNotRow#onMeasure(BigTextStyle)',
    'Should not be in the metric'
]


def add_main_thread_atrace(trace, ts, ts_end, buf, pid):
  trace.add_atrace_begin(ts=ts, tid=pid, pid=pid, buf=buf)
  trace.add_atrace_end(ts=ts_end, tid=pid, pid=pid)


def add_async_trace(trace, ts, ts_end, buf, pid):
  trace.add_atrace_async_begin(ts=ts, tid=pid, pid=pid, buf=buf)
  trace.add_atrace_async_end(ts=ts_end, tid=pid, pid=pid, buf=buf)


def add_binder_transaction(trace, tx_pid, rx_pid, start_ts, end_ts):
  trace.add_binder_transaction(
      transaction_id=tx_pid,
      ts_start=start_ts,
      ts_end=end_ts,
      tid=tx_pid,
      pid=tx_pid,
      reply_id=rx_pid,
      reply_ts_start=start_ts,
      reply_ts_end=end_ts,
      reply_tid=rx_pid,
      reply_pid=rx_pid)


# Adds a set of predefined blocking calls in places near the cuj boundaries to
# verify that only the portion inside the cuj is counted in the metric.
def add_cuj_with_blocking_calls(trace, cuj_name, pid):
  cuj_begin = 2_000_000
  cuj_dur = 15_000_000
  cuj_end = cuj_begin + cuj_dur
  blocking_call_name = "binder transaction"

  add_async_trace(trace, ts=cuj_begin, ts_end=cuj_end, buf=cuj_name, pid=pid)

  # all outside, before cuj, shouldn't be counted
  add_main_thread_atrace(
      trace,
      ts=cuj_begin - 2_000_000,
      ts_end=cuj_begin - 1_000_000,
      buf=blocking_call_name,
      pid=pid)

  # mid inside, mid outside. Should account for half the time.
  add_main_thread_atrace(
      trace,
      ts=cuj_begin - 1_000_000,
      ts_end=cuj_begin + 1_000_000,
      buf=blocking_call_name,
      pid=pid)

  # completely inside
  add_main_thread_atrace(
      trace,
      ts=cuj_begin + 2_000_000,
      ts_end=cuj_begin + 3_000_000,
      buf=blocking_call_name,
      pid=pid)

  add_main_thread_atrace(
      trace,
      ts=cuj_begin + 4_000_000,
      ts_end=cuj_begin + 7_000_000,
      buf=blocking_call_name,
      pid=pid)

  # mid inside, mid outside
  add_main_thread_atrace(
      trace,
      ts=cuj_end - 1_000_000,
      ts_end=cuj_end + 1_000_000,
      buf=blocking_call_name,
      pid=pid)

  # all outside, after cuj, shouldn't be counted/
  add_main_thread_atrace(
      trace,
      ts=cuj_end + 2_000_000,
      ts_end=cuj_end + 3_000_000,
      buf=blocking_call_name,
      pid=pid)


# Creates a cuj that contains one of each blocking call.
def add_all_blocking_calls_in_cuj(trace, pid):
  blocking_call_dur = 10_000_000
  blocking_call_ts = 2_000_000

  cuj_dur = len(blocking_call_names) * blocking_call_dur
  add_async_trace(
      trace,
      ts=blocking_call_ts,
      ts_end=blocking_call_ts + cuj_dur,
      buf="L<CUJ_WITH_MANY_BLOCKING_CALLS>",
      pid=pid)

  for blocking_call in blocking_call_names:
    add_main_thread_atrace(
        trace,
        ts=blocking_call_ts,
        ts_end=blocking_call_ts + blocking_call_dur,
        buf=blocking_call,
        pid=pid)
    blocking_call_ts += blocking_call_dur


# Creates 2 overlapping cuj, and a blocking call that lasts for both of them.
def add_overlapping_cujs_with_blocking_calls(trace, start_ts, pid):
  add_async_trace(
      trace,
      ts=start_ts,
      ts_end=start_ts + 10_000_000,
      buf="L<OVERLAPPING_CUJ_1>",
      pid=pid)
  add_async_trace(
      trace,
      ts=start_ts + 2_000_000,
      ts_end=start_ts + 12_000_000,
      buf="L<OVERLAPPING_CUJ_2>",
      pid=pid)

  add_main_thread_atrace(
      trace,
      ts=start_ts,
      ts_end=start_ts + 12_000_000,
      buf=blocking_call_names[0],
      pid=pid)


def add_cuj_with_named_binder_transaction(pid, rx_pid):
  cuj_begin = 40_000_000
  cuj_end = 50_000_000

  add_async_trace(
      trace,
      ts=cuj_begin,
      ts_end=cuj_end,
      buf="L<WITH_NAMED_BINDER_TRANSACTION>",
      pid=pid)

  add_binder_transaction(
      trace, tx_pid=pid, rx_pid=rx_pid, start_ts=cuj_begin, end_ts=cuj_end)

  # Slice inside the binder reply, to give a name to the binder call.
  # The named binder slice introduced should be the length of the entire
  # transaction even if the "name" slice only covers some of the binder reply.
  add_main_thread_atrace(
      trace,
      ts=cuj_begin + 1_000_000,
      ts_end=cuj_end - 1_000_000,
      buf="AIDL::java::IWindowManager::hasNavigationBar::server",
      pid=rx_pid)


def add_process(trace, package_name, uid, pid):
  trace.add_package_list(ts=0, name=package_name, uid=uid, version_code=1)
  trace.add_process(pid=pid, ppid=0, cmdline=package_name, uid=uid)
  trace.add_thread(tid=pid, tgid=pid, cmdline="MainThread", name="MainThread")


def setup_trace():
  trace = synth_common.create_trace()
  trace.add_packet()
  add_process(
      trace, package_name="com.android.systemui", uid=10001, pid=SYSUI_PID)
  add_process(
      trace,
      package_name="com.google.android.apps.nexuslauncher",
      uid=10002,
      pid=LAUNCHER_PID)
  add_process(
      trace,
      package_name="com.google.android.third.process",
      uid=10003,
      pid=THIRD_PROCESS_PID)
  trace.add_ftrace_packet(cpu=0)
  add_async_trace(trace, ts=0, ts_end=5, buf="J<IGNORED>", pid=SYSUI_PID)
  return trace


trace = setup_trace()

add_cuj_with_blocking_calls(trace, "L<TEST_SYSUI_LATENCY_EVENT>", pid=SYSUI_PID)
add_cuj_with_blocking_calls(
    trace, "L<TEST_LAUNCHER_LATENCY_EVENT>", pid=LAUNCHER_PID)

add_all_blocking_calls_in_cuj(trace, pid=THIRD_PROCESS_PID)

add_overlapping_cujs_with_blocking_calls(
    trace, pid=SYSUI_PID, start_ts=20_000_000)

add_cuj_with_named_binder_transaction(pid=SYSUI_PID, rx_pid=LAUNCHER_PID)

# Note that J<*> events are not tested here.
# See test_android_blocking_calls_on_jank_cujs.
sys.stdout.buffer.write(trace.trace.SerializeToString())
