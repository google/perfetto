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

APP_PID = 3
APP_TID = APP_PID
SECOND_APP_TID = 3
JIT_TID = 4
GC_TID = 5
GC2_TID = 6
BINDER_TID = 7
FONTS_TID = 8
SYSTEM_SERVER_PID = 2
SYSTEM_SERVER_TID = 2
LAUNCH_START_TS = 100
LAUNCH_END_TS = 10**9

trace = synth_common.create_trace()
trace.add_packet()
trace.add_process(1, 0, 'init')
trace.add_process(SYSTEM_SERVER_PID, 1, 'system_server')
trace.add_process(APP_PID, 1, 'com.some.app', uid=10001)
trace.add_thread(tid=SECOND_APP_TID, tgid=APP_PID, cmdline='second_thread')
trace.add_thread(
    tid=JIT_TID,
    tgid=APP_PID,
    cmdline='Jit thread pool',
    name='Jit thread pool')
trace.add_thread(
    tid=GC_TID, tgid=APP_PID, cmdline='HeapTaskDaemon', name='HeapTaskDaemon')
trace.add_thread(
    tid=GC2_TID, tgid=APP_PID, cmdline='HeapTaskDaemon', name='HeapTaskDaemon')
trace.add_thread(tid=BINDER_TID, tgid=APP_PID, cmdline='Binder', name='Binder')
trace.add_thread(tid=FONTS_TID, tgid=APP_PID, cmdline='fonts', name='fonts')

trace.add_package_list(ts=99, name='com.some.app', uid=10001, version_code=123)

trace.add_ftrace_packet(cpu=0)
# Start intent.
trace.add_atrace_begin(
    ts=LAUNCH_START_TS,
    pid=SYSTEM_SERVER_PID,
    tid=SYSTEM_SERVER_TID,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(
    ts=LAUNCH_START_TS + 1, tid=SYSTEM_SERVER_TID, pid=SYSTEM_SERVER_PID)

# System server launching the app.
trace.add_atrace_async_begin(
    ts=LAUNCH_START_TS + 2,
    pid=SYSTEM_SERVER_PID,
    tid=SYSTEM_SERVER_TID,
    buf='launching: com.some.app')

# Emulate a hot start (and therefore that we only see activityResume).
trace.add_atrace_begin(ts=125, tid=APP_TID, pid=APP_PID, buf='activityResume')
trace.add_atrace_end(ts=130, tid=APP_TID, pid=APP_PID)

# OpenDex slices within the startup.
trace.add_atrace_begin(
    ts=150, pid=APP_PID, tid=APP_TID, buf='OpenDexFilesFromOat(something)')
trace.add_atrace_end(ts=165, pid=APP_PID, tid=APP_TID)

trace.add_atrace_begin(
    ts=170, pid=APP_PID, tid=APP_TID, buf='OpenDexFilesFromOat(something else)')
trace.add_atrace_end(ts=5 * 10**8, pid=APP_PID, tid=APP_TID)

# OpenDex slice outside the startup.
trace.add_atrace_begin(
    ts=5, pid=APP_PID, tid=APP_TID, buf='OpenDexFilesFromOat(nothing)')
trace.add_atrace_end(ts=35, pid=APP_PID, tid=APP_TID)

# dlopen slices within the startup.
trace.add_atrace_begin(
    ts=166, pid=APP_PID, tid=APP_TID, buf='dlopen: libandroid.so')
trace.add_atrace_end(ts=167, pid=APP_PID, tid=APP_TID)

trace.add_atrace_begin(
    ts=168, pid=APP_PID, tid=APP_TID, buf='dlopen: libandroid2.so')
trace.add_atrace_end(ts=169, pid=APP_PID, tid=APP_TID)

trace.add_atrace_async_end(
    ts=LAUNCH_END_TS,
    tid=SYSTEM_SERVER_TID,
    pid=SYSTEM_SERVER_PID,
    buf='launching: com.some.app')

# VerifyClass slices within the startup.
trace.add_atrace_begin(ts=250, pid=APP_PID, tid=APP_TID, buf='VerifyClass vr')
trace.add_atrace_end(ts=265, pid=APP_PID, tid=APP_TID)

trace.add_atrace_begin(ts=270, pid=APP_PID, tid=APP_TID, buf='VerifyClass dl')
trace.add_atrace_end(ts=275, pid=APP_PID, tid=APP_TID)

# VerifyClass slice outside the startup.
trace.add_atrace_begin(ts=55, pid=APP_PID, tid=APP_TID, buf='VerifyClass xf')
trace.add_atrace_end(ts=65, pid=APP_PID, tid=APP_TID)

# VerifyClass slice on a different thread, overlapping with the other slices.
trace.add_atrace_begin(
    ts=260, pid=APP_PID, tid=SECOND_APP_TID, buf='VerifyClass vp')
trace.add_atrace_end(ts=280, pid=APP_PID, tid=SECOND_APP_TID)

# JIT compilation slices
trace.add_atrace_begin(
    ts=150, pid=APP_PID, tid=JIT_TID, buf='JIT compiling something')
trace.add_atrace_end(ts=160, pid=APP_PID, tid=JIT_TID)

trace.add_sched(ts=155, prev_pid=0, next_pid=JIT_TID)
trace.add_sched(ts=165, prev_pid=JIT_TID, next_pid=0)

trace.add_atrace_begin(
    ts=170, pid=APP_PID, tid=JIT_TID, buf='JIT compiling something else')
trace.add_atrace_end(ts=190, pid=APP_PID, tid=JIT_TID)

trace.add_sched(ts=170, prev_pid=0, next_pid=JIT_TID)
trace.add_sched(ts=175, prev_pid=JIT_TID, next_pid=0, prev_state='R')
trace.add_sched(ts=185, prev_pid=0, next_pid=JIT_TID)
trace.add_sched(ts=190, prev_pid=JIT_TID, next_pid=0)

# JIT slice, but not on JIT thread.
trace.add_atrace_begin(
    ts=200, pid=APP_PID, tid=SECOND_APP_TID, buf='JIT compiling nothing')
trace.add_atrace_end(ts=210, pid=APP_PID, tid=SECOND_APP_TID)

# Slice on JIT thread, but name doesn't match
trace.add_atrace_begin(
    ts=200, pid=APP_PID, tid=JIT_TID, buf='JIT compiled something')
trace.add_atrace_end(ts=210, pid=APP_PID, tid=JIT_TID)

# GC slices.
trace.add_atrace_begin(
    ts=300, pid=APP_PID, tid=GC_TID, buf='Background concurrent copying GC')
trace.add_atrace_end(ts=330, pid=APP_PID, tid=GC_TID)

trace.add_atrace_begin(
    ts=340, pid=APP_PID, tid=GC_TID, buf='CollectorTransition mark sweep GC')
trace.add_atrace_end(ts=390, pid=APP_PID, tid=GC_TID)

trace.add_atrace_begin(ts=320, pid=APP_PID, tid=GC2_TID, buf='semispace GC')
trace.add_atrace_end(ts=370, pid=APP_PID, tid=GC2_TID)

# Start running copying slice on the first thread
trace.add_sched(ts=310, prev_pid=0, next_pid=GC_TID)
# Switch to the second thread to run semispace slice
trace.add_sched(ts=325, prev_pid=GC_TID, next_pid=GC2_TID)
# Switch back to the first thread to run mark sweep slice
trace.add_sched(ts=350, prev_pid=GC2_TID, next_pid=GC_TID)
# Finish running for GC.
trace.add_sched(ts=360, prev_pid=GC_TID, next_pid=0)

# Long binder transactions.
trace.add_binder_transaction(1, 10**8, 2 * (10**8), BINDER_TID, APP_PID, 2,
                             10**8 + 1, 2 * (10**8) - 1, SYSTEM_SERVER_TID,
                             SYSTEM_SERVER_PID)

trace.add_binder_transaction(3, 3 * (10**8), 5 * (10**8), FONTS_TID, APP_PID, 4,
                             3 * (10**8) + 1, 5 * (10**8) - 1, BINDER_TID,
                             APP_PID)

# A short binder transaction.
trace.add_binder_transaction(5, 10**7, 5 * (10**7), APP_TID, APP_PID, 6,
                             10**7 + 1, 5 * (10**7) - 1, SYSTEM_SERVER_TID,
                             SYSTEM_SERVER_PID)

# Intent successful.
trace.add_atrace_begin(
    ts=LAUNCH_END_TS + 1,
    pid=SYSTEM_SERVER_PID,
    tid=SYSTEM_SERVER_TID,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(
    ts=LAUNCH_END_TS + 2, tid=SYSTEM_SERVER_TID, pid=SYSTEM_SERVER_PID)

sys.stdout.buffer.write(trace.trace.SerializeToString())
