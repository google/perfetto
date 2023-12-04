#!/usr/bin/env python3
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

import synth_common

trace = synth_common.create_trace()
trace.add_packet()
trace.add_process(1, 0, 'init')
trace.add_process(2, 1, 'lmkd')
trace.add_process(3, 1, 'com.google.android.calendar')

trace.add_ftrace_packet(cpu=0)
trace.add_oom_score_update(ts=100, oom_score_adj=0, pid=3)
trace.add_atrace_begin(ts=100000, pid=1, tid=1, buf='lmk,3,1,900')
trace.add_atrace_end(ts=100001, pid=1, tid=1)

sys.stdout.buffer.write(trace.trace.SerializeToString())
