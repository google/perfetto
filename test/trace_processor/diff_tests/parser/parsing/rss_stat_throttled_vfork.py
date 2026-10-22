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

# rss_stat_throttled version of the vfork sequence in rss_stat_mm_id_clone.py.
# While the child shares the parent's mm, its curr=true events for that mm are
# attributed to the parent which owns it. Once the child execs and reports its
# own mm, that mm is attributed to the child.

from os import sys

import synth_common

PARENT_MM = 0x1234
CHILD_MM = 0x5678

trace = synth_common.create_trace()

trace.add_packet(ts=1)
trace.add_process(10, 1, "parent_process")

trace.add_ftrace_packet(1)

trace.add_rss_stat_throttled(
    100, tid=10, member=0, size=100, mm_id=PARENT_MM, curr=1)

trace.add_newtask(
    101,
    tid=10,
    new_tid=11,
    new_comm="child_process",
    flags=synth_common.CLONE_VFORK | synth_common.CLONE_VM)

# Child modifies the shared (parent's) mm.
trace.add_rss_stat_throttled(
    102, tid=11, member=0, size=90, mm_id=PARENT_MM, curr=1)

# Child execs and reports its own mm.
trace.add_rss_stat_throttled(
    103, tid=11, member=0, size=10, mm_id=CHILD_MM, curr=1)

# Parent resumes.
trace.add_rss_stat_throttled(
    104, tid=10, member=0, size=95, mm_id=PARENT_MM, curr=1)

sys.stdout.buffer.write(trace.trace.SerializeToString())
