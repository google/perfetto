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

# This synthetic trace covers a live process being reparented onto a new parent
# (init) after its real parent exits. A later /proc scan then reports a
# different ppid for the same, still-alive pid. The process tracker must not
# mistake this for pid reuse.
#
# It exercises the per-parent disambiguation: a changed parent is treated as
# reparenting (one process) only when the previous parent is known to have
# exited. Without that exit event, the changed parent still looks like reuse
# (two processes), which is the best we can do.

from os import sys, path

import synth_common

trace = synth_common.create_trace()

# Scenario A: the parent's exit IS recorded.
# The parent process (pid 100) is seen first.
trace.add_packet(ts=1)
trace.add_process(100, 0, "parent_a")

# It forks a child (pid 200); the child's parent is pid 100.
trace.add_ftrace_packet(0)
trace.add_newtask(ts=10, tid=100, new_tid=200, new_comm='child_a', flags=0)
# The parent exits (do_exit) but isn't reaped; the child is reparented to init.
trace.add_process_exit(ts=20, pid=100, tid=100, comm='parent_a')

# A later /proc scan reports the child reparented onto init (pid 1).
trace.add_packet(ts=30)
trace.add_process(200, 1, "child_a")

# Scenario B: the parent's exit is NOT recorded.
# The parent process (pid 300) is seen first.
trace.add_packet(ts=2)
trace.add_process(300, 0, "parent_b")

# It forks a child (pid 400); the child's parent is pid 300.
trace.add_ftrace_packet(0)
trace.add_newtask(ts=11, tid=300, new_tid=400, new_comm='child_b', flags=0)
# No exit event for the parent.

# A later /proc scan reports the child with a different ppid (init).
trace.add_packet(ts=31)
trace.add_process(400, 1, "child_b")

sys.stdout.buffer.write(trace.trace.SerializeToString())
