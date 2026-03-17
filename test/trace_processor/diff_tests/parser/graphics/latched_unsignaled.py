#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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


class JankType:
  JANK_NONE = 1


class PresentType:
  PRESENT_ON_TIME = 1


class PredictionType:
  PREDICTION_VALID = 1


trace = synth_common.create_trace()

# 1. SurfaceFrame with latched_unsignaled = True
trace.add_expected_surface_frame_start_event(
    ts=10,
    cookie=1,
    token=1,
    display_frame_token=10,
    pid=1000,
    layer_name="Layer1")
trace.add_frame_end_event(ts=20, cookie=1)
trace.add_actual_surface_frame_start_event(
    ts=10,
    cookie=2,
    token=1,
    display_frame_token=10,
    pid=1000,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_NONE,
    prediction_type=PredictionType.PREDICTION_VALID,
    latched_unsignaled=True)
trace.add_frame_end_event(ts=20, cookie=2)

# 2. SurfaceFrame with latched_unsignaled = False
trace.add_expected_surface_frame_start_event(
    ts=30,
    cookie=3,
    token=2,
    display_frame_token=20,
    pid=1000,
    layer_name="Layer1")
trace.add_frame_end_event(ts=40, cookie=3)
trace.add_actual_surface_frame_start_event(
    ts=30,
    cookie=4,
    token=2,
    display_frame_token=20,
    pid=1000,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_NONE,
    prediction_type=PredictionType.PREDICTION_VALID,
    latched_unsignaled=False)
trace.add_frame_end_event(ts=40, cookie=4)

# 3. SurfaceFrame without latched_unsignaled (old trace)
trace.add_expected_surface_frame_start_event(
    ts=50,
    cookie=5,
    token=3,
    display_frame_token=30,
    pid=1000,
    layer_name="Layer1")
trace.add_frame_end_event(ts=60, cookie=5)
trace.add_actual_surface_frame_start_event(
    ts=50,
    cookie=6,
    token=3,
    display_frame_token=30,
    pid=1000,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_NONE,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=60, cookie=6)

sys.stdout.buffer.write(trace.trace.SerializeToString())
