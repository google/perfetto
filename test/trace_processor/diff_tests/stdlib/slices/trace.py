#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License a
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from os import sys

import synth_common

from synth_common import ms_to_ns

trace = synth_common.create_trace()

async_track_id = 1
process_track = 2
pid = 3
thread_track = 4
tid = 5
seq = 6

trace.add_track_descriptor(async_track_id)
trace.add_process_track_descriptor(
    process_track, pid=pid, process_name="Process")
trace.add_thread_track_descriptor(
    process_track, thread_track, tid=tid, pid=pid, thread_name="Thread")

trace.add_track_event_slice("AsyncSlice", ts=1, dur=2, track=async_track_id)
trace.add_track_event_slice("ProcessSlice", ts=3, dur=4, track=process_track)
trace.add_track_event_slice("ThreadSlice", ts=5, dur=8, track=thread_track)
trace.add_track_event_slice(
    "NestedThreadSlice", ts=6, dur=1, track=thread_track)

sys.stdout.buffer.write(trace.trace.SerializeToString())
