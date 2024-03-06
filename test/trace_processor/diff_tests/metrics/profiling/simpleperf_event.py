#!/usr/bin/env python3
# Copyright (C) 2022 The Android Open Source Project
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

trace.add_packet(ts=1)
pid_a = 10
trace.add_process(pid_a, 1, "process_a")
trace.add_thread(100, pid_a, "thread_a1", "thread_a1")
trace.add_thread(101, pid_a, "thread_a2", "thread_a2")

pid_b = 11
trace.add_process(pid_b, 10, "process_b")
trace.add_thread(110, pid_b, "thread_b1", "thread_b1")
trace.add_thread(111, pid_b, "thread_b2", "thread_b2")

pid_sp = 90
trace.add_process(pid_sp, 1, "simpleperf")

trace.add_ftrace_packet(1)

trace.add_atrace_counter(1300, pid_sp, pid_sp, 'instructions_tid100_cpu0', 1)
trace.add_atrace_counter(1200, pid_sp, pid_sp, 'instructions_tid100_cpu0', 3)
trace.add_atrace_counter(1100, pid_sp, pid_sp, 'instructions_tid100_cpu1', 2)
trace.add_atrace_counter(1000, pid_sp, pid_sp, 'instructions_tid101_cpu0', 5)

trace.add_atrace_counter(1100, pid_sp, pid_sp, 'instructions_tid111_cpu0', 2)

trace.add_atrace_counter(1000, pid_sp, pid_sp, 'cpu-cycles_tid101_cpu0', 10)
trace.add_atrace_counter(1200, pid_sp, pid_sp, 'cpu-cycles_tid101_cpu0', 7)

sys.stdout.buffer.write(trace.trace.SerializeToString())
