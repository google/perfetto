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

# Synthetic trace exercising rss_stat events emitted from the exit path of a
# process. During exit_mm() the kernel clears current->mm *before* calling
# mmput(), so the synchronous teardown emits rss_stat events with curr=false
# even though the mm still belongs (exclusively) to the exiting thread. Those
# events should be attributed to the exiting thread, not dropped.

from os import sys, path

import synth_common

trace = synth_common.create_trace()

trace.add_packet(ts=1)
trace.add_process(10, 1, "exiting_process")

trace.add_ftrace_packet(1)

# Normal rss_stat while the process is live: associates mm 0x1234 with tid 10.
trace.add_rss_stat(100, tid=10, member=0, size=100, mm_id=0x1234, curr=1)

# exit_mm() runs: current->mm is reset to NULL, then mmput() synchronously
# tears the mm down and emits rss_stat events with curr=false. These still
# describe tid 10's own mm and should be attributed to tid 10.
trace.add_rss_stat(101, tid=10, member=0, size=50, mm_id=0x1234, curr=0)
trace.add_rss_stat(102, tid=10, member=0, size=0, mm_id=0x1234, curr=0)

# Finally the scheduler frees the task.
trace.add_process_free(ts=103, tid=10, comm="exiting_process", prio=0)

sys.stdout.buffer.write(trace.trace.SerializeToString())
