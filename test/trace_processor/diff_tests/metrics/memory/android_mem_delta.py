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

file_member = 0
anon_member = 1
swap_member = 2

trace = synth_common.create_trace()
trace.add_packet()
trace.add_process(1, 0, 'com.my.pkg')

trace.add_ftrace_packet(cpu=0)
trace.add_rss_stat(100, 1, file_member, 10000)
trace.add_rss_stat(101, 1, file_member, 2000)
trace.add_rss_stat(102, 1, file_member, 5000)
trace.add_rss_stat(103, 1, file_member, 8000)
trace.add_rss_stat(104, 1, file_member, 9000)
trace.add_rss_stat(105, 1, file_member, 6000)

sys.stdout.buffer.write(trace.trace.SerializeToString())
