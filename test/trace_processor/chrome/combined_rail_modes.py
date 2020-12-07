#!/usr/bin/env python3
# Copyright (C) 2020 The Android Open Source Project
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

trace = synth_common.create_trace()

track1 = 1234
track2 = 4567

trace.add_track_descriptor(track1, "Renderer Thread 1", pid=0, tid=1)
trace.add_track_descriptor(track2, "Renderer Thread 2", pid=2, tid=3)

trace.add_rail_mode_slice(
    ts=0, dur=10, track=track1, mode=synth_common.RAIL_MODE_RESPONSE)
trace.add_rail_mode_slice(
    ts=10, dur=20, track=track1, mode=synth_common.RAIL_MODE_LOAD)
trace.add_rail_mode_slice(
    ts=30, dur=-1, track=track1, mode=synth_common.RAIL_MODE_IDLE)

trace.add_rail_mode_slice(
    ts=0, dur=10, track=track2, mode=synth_common.RAIL_MODE_ANIMATION)
trace.add_rail_mode_slice(
    ts=10, dur=25, track=track2, mode=synth_common.RAIL_MODE_IDLE)
trace.add_rail_mode_slice(
    ts=25, dur=10, track=track2, mode=synth_common.RAIL_MODE_ANIMATION)
trace.add_rail_mode_slice(
    ts=35, dur=10, track=track2, mode=synth_common.RAIL_MODE_IDLE)

sys.stdout.buffer.write(trace.trace.SerializeToString())
