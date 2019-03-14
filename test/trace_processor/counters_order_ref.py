#!/usr/bin/python
# Copyright (C) 2019 The Android Open Source Project
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

sys.path.append(path.dirname(path.abspath(__file__)))
import synth_common

trace = synth_common.create_trace()
trace.add_process_tree_packet()
trace.add_process(pid=1, ppid=0, cmdline="init")
trace.add_process(pid=2, ppid=1, cmdline="two_thread_process")
trace.add_process(pid=4, ppid=1, cmdline="single_thread_process")
trace.add_thread(tid=3, tgid=2, cmdline="two_thread_process")

trace.add_ftrace_packet(cpu=0)
trace.add_rss_stat(ts=1000, tid=4, member=0, size=200)
trace.add_rss_stat(ts=1005, tid=3, member=0, size=200)
trace.add_rss_stat(ts=1010, tid=5, member=0, size=100)

trace.add_oom_score_update(ts=1000, oom_score_adj=1000, pid=2)

print(trace.trace.SerializeToString())
