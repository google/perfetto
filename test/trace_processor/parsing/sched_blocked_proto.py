#!/usr/bin/env python3
# Copyright (C) 2020 The Android Open Source Project
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

file_member = 0
anon_member = 1

trace = synth_common.create_trace()
trace.add_packet()
trace.add_process(1, 0, "init")
trace.add_process(2, 0, "init2")
trace.add_process(3, 0, "unblocker")

trace.add_ftrace_packet(0)
trace.add_sched(
    ts=10,
    prev_pid=0,
    prev_comm='swapper',
    prev_state='R',
    next_pid=1,
    next_comm='foo')
trace.add_sched(ts=100, prev_pid=1, prev_state='U', next_pid=2, next_comm='bar')
trace.add_sched_blocked_reason(ts=101, pid=1, io_wait=0, unblock_pid=3)
trace.add_sched(ts=110, prev_pid=2, prev_state='U', next_pid=0)
trace.add_sched_blocked_reason(ts=111, pid=2, io_wait=1, unblock_pid=3)

sys.stdout.buffer.write(trace.trace.SerializeToString())
