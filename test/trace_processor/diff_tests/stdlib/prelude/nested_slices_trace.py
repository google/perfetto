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

track1_id = 1
track2_id = 2

trace.add_track_descriptor(track1_id)
trace.add_track_descriptor(track2_id)

trace.add_track_event_slice("Slice 1", ts=1, dur=10, track=track1_id)
trace.add_track_event_slice("Slice 2", ts=2, dur=3, track=track1_id)
trace.add_track_event_slice("Slice 3", ts=6, dur=3, track=track1_id)
trace.add_track_event_slice("Slice 4", ts=3, dur=1, track=track2_id)

sys.stdout.buffer.write(trace.trace.SerializeToString())
