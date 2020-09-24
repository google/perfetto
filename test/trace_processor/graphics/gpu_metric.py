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
trace.add_packet()
trace.add_process(1, 0, "app_1")
trace.add_process(2, 0, "app_2")
trace.add_process(3, 0, "app_2")

trace.add_ftrace_packet(cpu=0)

# max=4, min=1, avg=(2*1+5*2+(10-9)*4)/(10-2)=2
trace.add_gpu_mem_total_ftrace_event(pid=0, ts=2, size=1)
trace.add_gpu_mem_total_ftrace_event(pid=0, ts=4, size=2)
trace.add_gpu_mem_total_ftrace_event(pid=0, ts=9, size=4)

# max=8, min=2, avg=(5*2+(10-9)*8)/(10-4)=3
trace.add_gpu_mem_total_ftrace_event(pid=1, ts=4, size=2)
trace.add_gpu_mem_total_ftrace_event(pid=1, ts=9, size=8)

# max=8, min=6, avgxdur=2*6+(10-4)*8=60, dur=2+(10-4)=8
trace.add_gpu_mem_total_ftrace_event(pid=2, ts=2, size=6)
trace.add_gpu_mem_total_ftrace_event(pid=2, ts=4, size=8)

# max=10, min=7, avgxdur=1*7+(10-7)*10=37, dur=1+(10-7)=4
trace.add_gpu_mem_total_ftrace_event(pid=3, ts=6, size=7)
trace.add_gpu_mem_total_ftrace_event(pid=3, ts=7, size=10)

# app_2 will be aggregated
# max=10, min=6, avg=(60+37)/(8+4)=8

sys.stdout.buffer.write(trace.trace.SerializeToString())
