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

anon_member = 1
swap_member = 2

trace = synth_common.create_trace()
trace.add_process_tree_packet()
trace.add_process(1, 0, 'init')
trace.add_process(2, 1, 'system_server')
trace.add_process(3, 1, 'com.google.android.calendar')
trace.add_process(4, 1, 'com.google.android.calendar')

trace.add_ftrace_packet(cpu=0)
trace.add_rss_stat(100, 3, anon_member, 1000)
trace.add_rss_stat(100, 3, swap_member, 1000)
trace.add_rss_stat(100, 4, anon_member, 100)
trace.add_rss_stat(100, 4, swap_member, 50)

trace.add_rss_stat(200, 3, anon_member, 2000)
trace.add_rss_stat(200, 3, swap_member, 2000)
trace.add_rss_stat(200, 4, anon_member, 1000)
trace.add_rss_stat(200, 4, swap_member, 100)

trace.add_rss_stat(300, 3, anon_member, 3000)
trace.add_rss_stat(300, 3, swap_member, 3000)
trace.add_rss_stat(300, 4, anon_member, 50)
trace.add_rss_stat(300, 4, swap_member, 100)

print(trace.trace.SerializeToString())
