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
trace.add_process(10, 1, "parent_process")
trace.add_process(10335, 10, "child_process")
trace.add_thread(15000, 10335, "worker thread")

trace.add_ftrace_packet(1)

# unskipped validation
trace.add_atrace_begin(ts=100, tid=10335, pid=10335, buf="onMessageRefresh")
trace.add_atrace_begin(
    ts=200, tid=10335, pid=10335, buf="HwcPresentOrValidateDisplay 0")
trace.add_atrace_end(ts=300, tid=10335, pid=10335)
trace.add_atrace_begin(ts=400, tid=10335, pid=10335, buf="HwcPresentDisplay 0")
trace.add_atrace_end(ts=500, tid=10335, pid=10335)
trace.add_atrace_end(ts=600, tid=10335, pid=10335)

# skipped validation
trace.add_atrace_begin(ts=1_100, tid=10335, pid=10335, buf="composite 2")
trace.add_atrace_begin(
    ts=1_200, tid=10335, pid=10335, buf="HwcPresentOrValidateDisplay 0")
trace.add_atrace_end(ts=1_300, tid=10335, pid=10335)
trace.add_atrace_end(ts=1_400, tid=10335, pid=10335)

# separated validation where HwcValidateDisplay is executed from worker thread
trace.add_atrace_begin(ts=2_100, tid=10335, pid=10335, buf="composite 3")
trace.add_atrace_begin(ts=2_200, tid=15000, pid=10335, buf="otherFunction")
trace.add_atrace_begin(
    ts=2_300, tid=15000, pid=10335, buf="HwcValidateDisplay 1")
trace.add_atrace_end(ts=2_400, tid=15000, pid=10335)
trace.add_atrace_end(ts=2_500, tid=15000, pid=10335)
trace.add_atrace_begin(
    ts=2_600, tid=10335, pid=10335, buf="HwcPresentDisplay 1")
trace.add_atrace_end(ts=2_700, tid=10335, pid=10335)
trace.add_atrace_end(ts=2_800, tid=10335, pid=10335)

# skipped validation
trace.add_atrace_begin(ts=3_100, tid=10335, pid=10335, buf="AnotherFunction")
trace.add_atrace_begin(ts=3_200, tid=10335, pid=10335, buf="onMessageRefresh")
trace.add_atrace_begin(
    ts=3_300, tid=10335, pid=10335, buf="HwcPresentOrValidateDisplay 0")
trace.add_atrace_end(ts=3_400, tid=10335, pid=10335)
trace.add_atrace_end(ts=3_500, tid=10335, pid=10335)
trace.add_atrace_end(ts=3_600, tid=10335, pid=10335)

trace.add_atrace_begin(
    ts=3_700, tid=15000, pid=10335, buf="HwcPresentOrValidateDisplay 1")
trace.add_atrace_end(ts=3_800, tid=15000, pid=10335)

# incomplete (ignored)
trace.add_atrace_begin(
    ts=4_200, tid=10335, pid=10335, buf="HwcValidateDisplay 1")
trace.add_atrace_end(ts=4_300, tid=10335, pid=10335)

sys.stdout.buffer.write(trace.trace.SerializeToString())
