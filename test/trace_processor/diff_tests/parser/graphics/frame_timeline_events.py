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

# DisplayFrame without a SurfaceFrame
trace.add_expected_display_frame_start_event(ts=20, cookie=1, token=2, pid=666)
trace.add_frame_end_event(ts=26, cookie=1)
trace.add_actual_display_frame_start_event(
    ts=20,
    cookie=2,
    token=2,
    pid=666,
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_NONE,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=26, cookie=2)

# DisplayFrame with a SurfaceFrame
trace.add_expected_display_frame_start_event(ts=40, cookie=3, token=4, pid=666)
trace.add_frame_end_event(ts=46, cookie=3)
trace.add_actual_display_frame_start_event(
    ts=42,
    cookie=4,
    token=4,
    pid=666,
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_NONE,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=47, cookie=4)
trace.add_expected_surface_frame_start_event(
    ts=21,
    cookie=5,
    token=1,
    display_frame_token=4,
    pid=1000,
    layer_name="Layer1")
trace.add_frame_end_event(ts=36, cookie=5)
trace.add_actual_surface_frame_start_event(
    ts=21,
    cookie=6,
    token=1,
    display_frame_token=4,
    pid=1000,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_NONE,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=37, cookie=6)

# DisplayFrame with a janky SurfaceFrame
trace.add_expected_display_frame_start_event(ts=80, cookie=7, token=6, pid=666)
trace.add_frame_end_event(ts=86, cookie=7)
trace.add_actual_display_frame_start_event(
    ts=81,
    cookie=8,
    token=6,
    pid=666,
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_NONE,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=88, cookie=8)
trace.add_expected_surface_frame_start_event(
    ts=41,
    cookie=9,
    token=5,
    display_frame_token=6,
    pid=1000,
    layer_name="Layer1")
trace.add_frame_end_event(ts=56, cookie=9)
trace.add_actual_surface_frame_start_event(
    ts=41,
    cookie=10,
    token=5,
    display_frame_token=6,
    pid=1000,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_LATE,
    on_time_finish=0,
    gpu_composition=0,
    jank_type=JankType.JANK_APP_DEADLINE_MISSED,
    jank_severity_type=JankSeverityType.FULL,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=74, cookie=10)

# Janky DisplayFrame with a SurfaceFrame
trace.add_expected_display_frame_start_event(
    ts=120, cookie=11, token=8, pid=666)
trace.add_frame_end_event(ts=126, cookie=11)
trace.add_actual_display_frame_start_event(
    ts=108,
    cookie=12,
    token=8,
    pid=666,
    present_type=PresentType.PRESENT_EARLY,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_SF_SCHEDULING,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=112, cookie=12)
trace.add_expected_surface_frame_start_event(
    ts=90,
    cookie=13,
    token=7,
    display_frame_token=8,
    pid=1000,
    layer_name="Layer1")
trace.add_frame_end_event(ts=106, cookie=13)
trace.add_actual_surface_frame_start_event(
    ts=90,
    cookie=14,
    token=7,
    display_frame_token=8,
    pid=1000,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_EARLY,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_SF_SCHEDULING,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=106, cookie=14)

# DisplayFrame with multiple jank reasons
trace.add_expected_display_frame_start_event(
    ts=140, cookie=15, token=12, pid=666)
trace.add_frame_end_event(ts=146, cookie=15)
trace.add_actual_display_frame_start_event(
    ts=148,
    cookie=16,
    token=12,
    pid=666,
    present_type=PresentType.PRESENT_LATE,
    on_time_finish=0,
    gpu_composition=0,
    jank_type=JankType.JANK_SF_CPU_DEADLINE_MISSED
    | JankType.JANK_SF_SCHEDULING,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=156, cookie=16)

# Two SurfaceFrames with same token
trace.add_expected_display_frame_start_event(
    ts=170, cookie=17, token=15, pid=666)
trace.add_frame_end_event(ts=176, cookie=17)
trace.add_actual_display_frame_start_event(
    ts=170,
    cookie=18,
    token=15,
    pid=666,
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_NONE,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=176, cookie=18)
trace.add_expected_surface_frame_start_event(
    ts=150,
    cookie=19,
    token=14,
    display_frame_token=15,
    pid=1000,
    layer_name="Layer1")
trace.add_frame_end_event(ts=170, cookie=19)
trace.add_actual_surface_frame_start_event(
    ts=150,
    cookie=20,
    token=14,
    display_frame_token=15,
    pid=1000,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_NONE,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=167, cookie=20)
trace.add_expected_surface_frame_start_event(
    ts=150,
    cookie=21,
    token=14,
    display_frame_token=15,
    pid=1000,
    layer_name="Layer2")
trace.add_frame_end_event(ts=170, cookie=21)
trace.add_actual_surface_frame_start_event(
    ts=150,
    cookie=22,
    token=14,
    display_frame_token=15,
    pid=1000,
    layer_name="Layer2",
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_NONE,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=167, cookie=22)

# SurfaceFrame with prediction expired (no expected timeline packet)
trace.add_expected_display_frame_start_event(
    ts=200, cookie=23, token=17, pid=666)
trace.add_frame_end_event(ts=206, cookie=23)
trace.add_actual_display_frame_start_event(
    ts=200,
    cookie=24,
    token=17,
    pid=666,
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_NONE,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=206, cookie=24)
trace.add_actual_surface_frame_start_event(
    ts=80,
    cookie=25,
    token=16,
    display_frame_token=17,
    pid=1000,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_UNKNOWN,
    on_time_finish=0,
    gpu_composition=0,
    jank_type=JankType.JANK_UNKNOWN,
    jank_severity_type=JankSeverityType.PARTIAL,
    prediction_type=PredictionType.PREDICTION_EXPIRED)
trace.add_frame_end_event(ts=190, cookie=25)

# DisplayFrame with SF Stuffing jank
trace.add_expected_display_frame_start_event(
    ts=220, cookie=26, token=18, pid=666)
trace.add_frame_end_event(ts=230, cookie=26)
trace.add_actual_display_frame_start_event(
    ts=245,
    cookie=27,
    token=18,
    pid=666,
    present_type=PresentType.PRESENT_LATE,
    on_time_finish=0,
    gpu_composition=0,
    jank_type=JankType.JANK_SF_STUFFING,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=260, cookie=27)

# DisplayFrame with dropped frame jank
trace.add_expected_display_frame_start_event(
    ts=220, cookie=26, token=18, pid=666)
trace.add_frame_end_event(ts=230, cookie=26)
trace.add_actual_display_frame_start_event(
    ts=245,
    cookie=27,
    token=18,
    pid=666,
    present_type=PresentType.PRESENT_DROPPED,
    on_time_finish=0,
    gpu_composition=0,
    jank_type=JankType.JANK_DROPPED,
    prediction_type=PredictionType.PREDICTION_UNSPECIFIED)
trace.add_frame_end_event(ts=260, cookie=27)

sys.stdout.buffer.write(trace.trace.SerializeToString())
