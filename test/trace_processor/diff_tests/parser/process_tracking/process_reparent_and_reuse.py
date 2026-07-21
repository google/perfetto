#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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

# Scenario 1: reparenting. Child 200 is first seen under parent 100, then under
# init (pid 1).
trace.add_packet(ts=1)
trace.add_process(100, 1, "parent_a")
trace.add_process(200, 100, "child_a")

trace.add_packet(ts=30)
trace.add_process(200, 1, "child_a")

# Scenario 2: pid reuse. Process 300 is ended by sched_process_free, then the
# pid reappears as an unrelated process.
trace.add_packet(ts=2)
trace.add_process(300, 1, "orig_b")

trace.add_ftrace_packet(0)
trace.add_process_free(ts=20, tid=300, comm="orig_b", prio=0)

trace.add_packet(ts=40)
trace.add_process(300, 2000, "reused_b")

sys.stdout.buffer.write(trace.trace.SerializeToString())
