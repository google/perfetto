#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

from os import sys

import synth_common

from synth_common import ms_to_ns

trace = synth_common.create_trace()

process_track = 1
pid = 2
thread_track = 3
tid = 4

trace.add_process_track_descriptor(
    process_track, pid=pid, process_name="Process")
trace.add_thread_track_descriptor(
    process_track, thread_track, tid=tid, pid=pid, thread_name="Thread")

trace.add_track_event_slice(
    "ProcessSliceNoThread", ts=3, dur=100, track=process_track)
trace.add_track_event_slice("ThreadSlice1", ts=5, dur=200, track=thread_track)
trace.add_track_event_slice("ThreadSlice2", ts=6, dur=300, track=thread_track)

trace.add_track_event_slice(
    "ProcessSliceNoThread", ts=33, dur=10, track=process_track)
trace.add_track_event_slice("ThreadSlice1", ts=35, dur=10, track=thread_track)
trace.add_track_event_slice("ThreadSlice2", ts=36, dur=10, track=thread_track)

sys.stdout.buffer.write(trace.trace.SerializeToString())
