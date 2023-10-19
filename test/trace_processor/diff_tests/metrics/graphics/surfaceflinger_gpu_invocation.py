#!/usr/bin/env python3
# Copyright (C) 2021 The Android Open Source Project
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

# This synthetic trace tests handling of the mm_id field in the rss_stat
# event when mm_structs are reused on process death.

from os import sys, path

import synth_common

trace = synth_common.create_trace()

trace.add_packet(ts=1)
trace.add_process(
    pid=10, ppid=1, cmdline='/system/bin/surfaceflinger', uid=None)
trace.add_thread(tid=10, tgid=10, cmdline='', name='Main thread')
trace.add_thread(tid=33, tgid=10, cmdline='', name='GPU completion')
trace.add_ftrace_packet(1)

trace.add_atrace_begin(
    ts=1_000_000, tid=33, pid=10, buf='waiting for GPU completion 4')
trace.add_atrace_end(ts=2_000_000, tid=33, pid=10)

trace.add_atrace_begin(
    ts=3_000_000, tid=10, pid=10, buf='Trace GPU completion fence 5')
trace.add_atrace_begin(
    ts=3_000_000, tid=33, pid=10, buf='waiting for GPU completion 5')
trace.add_atrace_end(ts=3_000_500, tid=10, pid=10)
trace.add_atrace_end(ts=6_000_000, tid=33, pid=10)

trace.add_atrace_begin(
    ts=7_000_000, tid=10, pid=10, buf='Trace GPU completion fence 6')
trace.add_atrace_begin(
    ts=7_000_000, tid=33, pid=10, buf='waiting for GPU completion 6')
trace.add_atrace_end(ts=7_000_500, tid=10, pid=10)
trace.add_atrace_begin(
    ts=10_000_000, tid=10, pid=10, buf='Trace GPU completion fence 7')
trace.add_atrace_end(ts=10_000_500, tid=10, pid=10)
trace.add_atrace_end(ts=12_000_000, tid=33, pid=10)
trace.add_atrace_begin(
    ts=12_000_000, tid=33, pid=10, buf='waiting for GPU completion 7')
trace.add_atrace_end(ts=14_000_000, tid=33, pid=10)

trace.add_atrace_begin(
    ts=15_000_000, tid=10, pid=10, buf='Trace GPU completion fence 8')
trace.add_atrace_end(ts=15_000_500, tid=10, pid=10)

sys.stdout.buffer.write(trace.trace.SerializeToString())
