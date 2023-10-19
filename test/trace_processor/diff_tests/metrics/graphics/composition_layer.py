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
trace.add_process(11, 10, "child_process")

trace.add_ftrace_packet(1)

trace.add_print(
    ts=99, tid=11, buf='C|10|HWComposer: Total Layer for PrimaryDisplay(0)|7')
trace.add_print(
    ts=100, tid=11, buf='C|10|HWComposer: Total Layer for PrimaryDisplay(0)|5')
trace.add_print(
    ts=101, tid=11, buf='C|10|HWComposer: Total Layer for PrimaryDisplay(0)|6')
trace.add_print(
    ts=102, tid=11, buf='C|10|HWComposer: Total Layer for PrimaryDisplay(0)|0')
trace.add_print(
    ts=103, tid=11, buf='C|10|HWComposer: Total Layer for PrimaryDisplay(0)|4')
trace.add_print(
    ts=104, tid=11, buf='C|10|HWComposer: Total Layer for PrimaryDisplay(0)|6')
trace.add_print(
    ts=105, tid=11, buf='C|10|HWComposer: Total Layer for PrimaryDisplay(0)|0')
trace.add_print(
    ts=106, tid=11, buf='C|10|HWComposer: Total Layer for PrimaryDisplay(0)|4')
trace.add_print(
    ts=107, tid=11, buf='C|10|HWComposer: Total Layer for PrimaryDisplay(0)|5')
trace.add_print(
    ts=108, tid=11, buf='C|10|HWComposer: Total Layer for PrimaryDisplay(0)|0')
trace.add_print(
    ts=108, tid=11, buf='C|10|HWComposer: Total Layer for PrimaryDisplay(0)|3')
trace.add_print(
    ts=108, tid=11, buf='C|10|HWComposer: Total Layer for PrimaryDisplay(0)|0')

trace.add_print(
    ts=109,
    tid=11,
    buf='C|10|HWComposer: Total Layer for SecondaryDisplay(1)|5')

sys.stdout.buffer.write(trace.trace.SerializeToString())
