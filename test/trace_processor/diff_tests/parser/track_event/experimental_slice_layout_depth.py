#!/usr/bin/env python3
# Copyright (C) 2021 The Android Open Source Project
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

# This is intended to test the layout depth of async slices starting and
# ending at the same time (see b/189222451).

from os import sys

import synth_common

from synth_common import ms_to_ns

trace = synth_common.create_trace()

track1 = 1234
track2 = 1235
track3 = 1236

trace.add_track_descriptor(track1)
trace.add_track_descriptor(track2)
trace.add_track_descriptor(track3)

trace.add_track_event_slice(
    "AsyncSlice", ts=ms_to_ns(0), dur=ms_to_ns(10), track=track1)

trace.add_track_event_slice(
    "AsyncSlice", ts=ms_to_ns(10), dur=ms_to_ns(10), track=track2)

trace.add_track_event_slice(
    "AsyncSlice", ts=ms_to_ns(20), dur=ms_to_ns(10), track=track3)

sys.stdout.buffer.write(trace.trace.SerializeToString())
