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

# This synthetic trace tests handling of the mm_id field in the rss_stat
# event when mm_structs are reused on process death.

from os import sys, path

import synth_common

trace = synth_common.create_trace()

trace.add_packet(ts=1)
trace.add_process(10, 1, "parent_process")
trace.add_process(11, 10, "child_process")

trace.add_ftrace_packet(1)

trace.add_print(ts=99_000_000, tid=11, buf='C|10|panel_fps|60')
trace.add_print(ts=100_000_000, tid=11, buf='C|10|panel_fps|90')
trace.add_print(ts=101_000_000, tid=11, buf='C|10|panel_fps|60')
trace.add_print(ts=102_000_000, tid=11, buf='C|10|panel_fps|120')

# The duplicated fps will be ignored
trace.add_print(ts=103_000_000, tid=11, buf='C|10|panel_fps|120')

trace.add_print(ts=104_000_000, tid=11, buf='C|10|panel_fps|90')

# The last fps and its following duplicates will be ignored, and will
# only be used for the calculation of duration of the previous fps
trace.add_print(ts=105_000_000, tid=11, buf='C|10|panel_fps|24')
trace.add_print(ts=106_000_000, tid=11, buf='C|10|panel_fps|24')

trace.add_track_event_slice(
    "DisplayPowerController#updatePowerState",
    0,
    5000000,
    trusted_sequence_id=1)

trace.add_track_event_slice(
    "DisplayPowerController#updatePowerState",
    0,
    3000000,
    trusted_sequence_id=1)

sys.stdout.buffer.write(trace.trace.SerializeToString())
