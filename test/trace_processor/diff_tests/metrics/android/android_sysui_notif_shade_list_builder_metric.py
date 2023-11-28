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

# duration for each child slice
child_slice_dur = 10_000_000

# parent slice idle duration
parent_slice_idle_dur = 10_000_000

# List of interesting slices
parent_slice_names = [
    'ShadeListBuilder.buildList',
    'ShadeListBuilder.buildList',
    'Should not be in the metric',
]

# List of inflation-related descendant slices of interesting slices
inflation_child_slice_names = [
    'HybridGroupManager#inflateHybridView',
    'NotifChildCont#recreateHeader',
]

# List of Shade-node-modification-related descendant slices of interesting slices
modification_child_slice_names = [
    'ShadeNode#addChildAt',
    'ShadeNode#removeChildAt',
    'ShadeNode#moveChildTo',
]


def add_main_thread_atrace(trace, ts, ts_end, buf, pid):
    trace.add_atrace_begin(ts=ts, tid=pid, pid=pid, buf=buf)
    trace.add_atrace_end(ts=ts_end, tid=pid, pid=pid)


# Creates a trace that has the interesting slices that we are querying for
# A ShadeListBuilder.buildList slice that has one of each of the inflation_child_slice_names
# A ShadeListBuilder.buildList slice that has one of each of the modification_child_slice_names
def add_slices(trace, pid):
    slice_ts = 2_000_000
    slice_ts = add_slice_with_children(trace, pid, slice_ts, 'ShadeListBuilder.buildList', inflation_child_slice_names)
    add_slice_with_children(trace, pid, slice_ts, 'ShadeListBuilder.buildList', modification_child_slice_names)

# Add a slice with a set of children slices, return the parent slice's end ts
def add_slice_with_children(trace, pid, current_ts, parent_name, children_list):
    ts_end = current_ts + parent_slice_idle_dur + len(children_list) * (child_slice_dur + 1)
    # add the parent slice
    add_main_thread_atrace(
        trace,
        ts=current_ts,
        ts_end=ts_end,
        buf=parent_name,
        pid=pid)
    current_ts += parent_slice_idle_dur
    # Add the children
    for child_name in children_list:
        ts_child_end = current_ts + child_slice_dur + 1
        add_main_thread_atrace(
            trace,
            ts=current_ts,
            ts_end=ts_child_end,
            buf=child_name,
            pid=pid)
        current_ts = ts_child_end
    return ts_end

def add_process(trace, package_name, uid, pid):
    trace.add_package_list(ts=0, name=package_name, uid=uid, version_code=1)
    trace.add_process(
        pid=pid, ppid=0, cmdline=package_name, uid=uid)
    trace.add_thread(tid=pid, tgid=pid, cmdline="MainThread", name="MainThread")


def setup_trace():
    trace = synth_common.create_trace()
    trace.add_packet()
    add_process(trace, package_name="com.android.systemui", uid=10001,
                pid=SYSUI_PID)
    trace.add_ftrace_packet(cpu=0)
    return trace


trace = setup_trace()


add_slices(trace, pid=SYSUI_PID)

# See test_sysui_notif_shade_list_builder.
sys.stdout.buffer.write(trace.trace.SerializeToString())