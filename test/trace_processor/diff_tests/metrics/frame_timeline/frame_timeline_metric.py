#!/usr/bin/env python3
# Copyright (C) 2022 The Android Open Source Project
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

trace.add_packet(ts=5000000)
trace.add_process(1001, 0, "process1")
trace.add_process(1002, 0, "process2")
trace.add_process(1003, 0, "process3")
trace.add_process(1004, 0, "process4")

trace.add_actual_surface_frame_start_event(
    ts=21000000,
    cookie=6,
    token=100201,
    display_frame_token=100211,
    pid=1002,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_NONE,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=37000000, cookie=6)

trace.add_actual_surface_frame_start_event(
    ts=31000000,
    cookie=7,
    token=100202,
    display_frame_token=100212,
    pid=1002,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_APP_DEADLINE_MISSED,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=47000000, cookie=7)
trace.add_actual_surface_frame_start_event(
    ts=32000000,
    cookie=8,
    token=100202,
    display_frame_token=100212,
    pid=1002,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_ON_TIME,
    on_time_finish=1,
    gpu_composition=0,
    jank_type=JankType.JANK_APP_DEADLINE_MISSED,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=40000000, cookie=8)

# DisplayFrame with a janky SurfaceFrame
trace.add_actual_surface_frame_start_event(
    ts=41000000,
    cookie=10,
    token=100101,
    display_frame_token=100111,
    pid=1001,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_LATE,
    on_time_finish=0,
    gpu_composition=0,
    jank_type=JankType.JANK_APP_DEADLINE_MISSED,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=74000000, cookie=10)
trace.add_actual_surface_frame_start_event(
    ts=41000000,
    cookie=11,
    token=100102,
    display_frame_token=100112,
    pid=1001,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_LATE,
    on_time_finish=0,
    gpu_composition=0,
    jank_type=JankType.JANK_APP_DEADLINE_MISSED | JankType.JANK_BUFFER_STUFFING,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=75000000, cookie=11)

trace.add_actual_surface_frame_start_event(
    ts=81000000,
    cookie=15,
    token=100301,
    display_frame_token=100311,
    pid=1003,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_LATE,
    on_time_finish=0,
    gpu_composition=0,
    jank_type=JankType.JANK_SF_CPU_DEADLINE_MISSED,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=95000000, cookie=15)
trace.add_actual_surface_frame_start_event(
    ts=90000000,
    cookie=16,
    token=100302,
    display_frame_token=100312,
    pid=1003,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_DROPPED,
    on_time_finish=0,
    gpu_composition=0,
    jank_type=JankType.JANK_DROPPED,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=96000000, cookie=16)

trace.add_actual_surface_frame_start_event(
    ts=10000000,
    cookie=20,
    token=100402,
    display_frame_token=100412,
    pid=1004,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_DROPPED,
    on_time_finish=0,
    gpu_composition=0,
    jank_type=JankType.JANK_SF_STUFFING,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=12000000, cookie=20)

trace.add_actual_surface_frame_start_event(
    ts=12500000,
    cookie=25,
    token=100502,
    display_frame_token=100512,
    pid=1004,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_DROPPED,
    on_time_finish=0,
    gpu_composition=0,
    jank_type=JankType.JANK_SF_SCHEDULING,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=14000000, cookie=25)

trace.add_actual_surface_frame_start_event(
    ts=14500000,
    cookie=30,
    token=100602,
    display_frame_token=100612,
    pid=1004,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_DROPPED,
    on_time_finish=0,
    gpu_composition=0,
    jank_type=JankType.JANK_SF_CPU_DEADLINE_MISSED,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=15000000, cookie=30)

trace.add_actual_surface_frame_start_event(
    ts=15500000,
    cookie=35,
    token=100702,
    display_frame_token=100712,
    pid=1004,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_DROPPED,
    on_time_finish=0,
    gpu_composition=0,
    jank_type=JankType.JANK_SF_GPU_DEADLINE_MISSED,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=16000000, cookie=35)

trace.add_actual_surface_frame_start_event(
    ts=16500000,
    cookie=40,
    token=100802,
    display_frame_token=100812,
    pid=1004,
    layer_name="Layer1",
    present_type=PresentType.PRESENT_DROPPED,
    on_time_finish=0,
    gpu_composition=0,
    jank_type=JankType.JANK_SF_SCHEDULING | JankType.JANK_SF_STUFFING,
    prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=17000000, cookie=40)

sys.stdout.buffer.write(trace.trace.SerializeToString())
