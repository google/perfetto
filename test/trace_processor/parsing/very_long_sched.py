#!/usr/bin/env python3
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

import synth_common

trace = synth_common.create_trace()

trace.add_packet(ts=1)
trace.add_process(10, 0, "processa")
trace.add_process(20, 0, "processb")

trace.add_ftrace_packet(0)

# Add a very long (~1 month long) sched slice.
trace.add_sched(ts=50, prev_pid=10, next_pid=20)

end_ts = 1 * 30 * 24 * 60 * 60 * 60 * 1000 * 1000 * 1000
trace.add_sched(ts=end_ts, prev_pid=20, next_pid=10)

sys.stdout.buffer.write(trace.trace.SerializeToString())
