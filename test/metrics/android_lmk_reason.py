#!/usr/bin/python
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

sys.path.append(path.dirname(path.dirname(path.abspath(__file__))))
import synth_common

trace = synth_common.create_trace()

trace.add_process_tree_packet()
trace.add_process(1, 0, 'init')
trace.add_process(2, 1, 'system_server')
trace.add_process(3, 1, 'lmk_victim:no_data:ignored')
trace.add_process(4, 1, 'lmk_victim:no_ion')
trace.add_process(5, 1, 'lmk_victim:with_ion')

trace.add_ftrace_packet(cpu=0)
trace.add_kernel_lmk(ts=101, tid=3)

trace.add_ftrace_packet(cpu=0)
trace.add_oom_score_update(ts=201, oom_score_adj=0, pid=4)
trace.add_kernel_lmk(ts=202, tid=4)

trace.add_ftrace_packet(cpu=0)
trace.add_ion_event(ts=301, tid=5, heap_name='system', size=1000)
trace.add_oom_score_update(ts=302, oom_score_adj=100, pid=5)
trace.add_kernel_lmk(ts=303, tid=5)

# Dummy trace event to ensure the trace does not end on an LMK.
trace.add_ftrace_packet(cpu=0)
trace.add_oom_score_update(ts=1001, oom_score_adj=-800, pid=2)

print(trace.trace.SerializeToString())
