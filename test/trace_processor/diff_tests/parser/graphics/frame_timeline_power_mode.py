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

from os import sys, path

import synth_common


class JankType:
  JANK_UNSPECIFIED = 0
  JANK_NONE = 1
  JANK_SF_SCHEDULING = 2
  JANK_PREDICTION_ERROR = 4
  JANK_DISPLAY_HAL = 8
  JANK_SF_CPU_DEADLINE_MISSED = 16
  JANK_SF_GPU_DEADLINE_MISSED = 32
  JANK_APP_DEADLINE_MISSED = 64
  JANK_BUFFER_STUFFING = 128
  JANK_UNKNOWN = 256
  JANK_SF_STUFFING = 512
  JANK_DROPPED = 1024
  JANK_NON_ANIMATING = 2048
  JANK_APP_RESYNCED_JITTER = 4096
  JANK_DISPLAY_NOT_ON = 8192
  JANK_DISPLAY_MODE_CHANGE_IN_PROGRESS = 16384
  JANK_DISPLAY_POWER_MODE_CHANGE_IN_PROGRESS = 32768


class JankSeverityType:
  UNKNOWN = 0
  NONE = 1
  PARTIAL = 2
  FULL = 3


class PresentType:
  PRESENT_UNSPECIFIED = 0
  PRESENT_ON_TIME = 1
  PRESENT_LATE = 2
  PRESENT_EARLY = 3
  PRESENT_DROPPED = 4
  PRESENT_UNKNOWN = 5


class PredictionType:
  PREDICTION_UNSPECIFIED = 0
  PREDICTION_VALID = 1
  PREDICTION_EXPIRED = 2
  PREDICTION_UNKNOWN = 3


trace = synth_common.create_trace()

# DisplayFrame with Power Mode Change
trace.add_expected_display_frame_start_event(ts=100, cookie=1, token=1, pid=666)
trace.add_frame_end_event(ts=110, cookie=1)
trace.add_actual_display_frame_start_event(
    ts=100,
    cookie=2,
    token=1,
    pid=666,
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_DISPLAY_POWER_MODE_CHANGE_IN_PROGRESS,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=110, cookie=2)

# SurfaceFrame with Power Mode Change
trace.add_expected_surface_frame_start_event(
    ts=120,
    cookie=3,
    token=2,
    display_frame_token=1,
    pid=1000,
    layer_name="Layer1")
trace.add_frame_end_event(ts=130, cookie=3)
trace.add_actual_surface_frame_start_event(
    ts=120,
    cookie=4,
    token=2,
    display_frame_token=1,
    pid=1000,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_DISPLAY_POWER_MODE_CHANGE_IN_PROGRESS,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=135, cookie=4)

# DisplayFrame with Non Animating
trace.add_expected_display_frame_start_event(ts=140, cookie=5, token=3, pid=666)
trace.add_frame_end_event(ts=150, cookie=5)
trace.add_actual_display_frame_start_event(
    ts=140,
    cookie=6,
    token=3,
    pid=666,
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_NON_ANIMATING,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=150, cookie=6)

# DisplayFrame with Display Not ON
trace.add_expected_display_frame_start_event(ts=160, cookie=7, token=4, pid=666)
trace.add_frame_end_event(ts=170, cookie=7)
trace.add_actual_display_frame_start_event(
    ts=160,
    cookie=8,
    token=4,
    pid=666,
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_DISPLAY_NOT_ON,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=170, cookie=8)

sys.stdout.buffer.write(trace.trace.SerializeToString())
