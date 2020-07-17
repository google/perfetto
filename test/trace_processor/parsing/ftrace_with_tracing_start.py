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

trace = synth_common.create_trace()

# Add a tracing_started packet which should cause all ftrace
# events before this ts to be dropped.
packet = trace.add_packet(ts=100)
packet.service_event.tracing_started = True

# Everything in this packet should be dropped.
trace.add_ftrace_packet(cpu=0)
trace.add_sched(ts=50, prev_pid=1, next_pid=2, prev_comm='t1', next_comm='t2')
trace.add_sched(ts=60, prev_pid=2, next_pid=1, prev_comm='t2', next_comm='t1')
trace.add_sched(ts=70, prev_pid=1, next_pid=2, prev_comm='t1', next_comm='t2')
trace.add_sched(
    ts=80, prev_pid=2, next_pid=0, prev_comm='t2', next_comm='swapper')

# The first 2 slices here should also be dropped but the last one should be
# retained.
trace.add_ftrace_packet(cpu=2)
trace.add_sched(
    ts=80, prev_pid=0, next_pid=1, prev_comm='swapper', next_comm='t1')
trace.add_sched(ts=90, prev_pid=1, next_pid=2, prev_comm='t1', next_comm='t2')
trace.add_sched(ts=100, prev_pid=2, next_pid=1, prev_comm='t2', next_comm='t1')
trace.add_sched(ts=110, prev_pid=1, next_pid=2, prev_comm='t1', next_comm='t2')

sys.stdout.buffer.write(trace.trace.SerializeToString())
