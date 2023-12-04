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

trace.add_packet()
trace.add_process(pid=1, ppid=0, cmdline="p1")
trace.add_process(pid=2, ppid=1, cmdline="p2")

trace.add_thread(tid=1, tgid=1, cmdline='p1', name='p1')
trace.add_thread(tid=2, tgid=2, cmdline='p2', name='p2')

trace.add_ftrace_packet(cpu=0)
trace.add_print(ts=50, tid=1, buf='G|1|track|ev|1024\n')
trace.add_print(ts=55, tid=1, buf='G|1|track|ev|2048\n')
trace.add_print(ts=60, tid=2, buf='G|2|track|ev|1024\n')
trace.add_print(ts=65, tid=2, buf='H|2|track|ev|1024\n')
trace.add_print(ts=70, tid=1, buf='H|1|track|2048\n')
trace.add_print(ts=75, tid=1, buf='H|1|track|ev|1024\n')

sys.stdout.buffer.write(trace.trace.SerializeToString())
