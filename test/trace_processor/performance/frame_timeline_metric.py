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
  JANK_UNSPECIFIED = 0;
  JANK_NONE = 1;
  JANK_SF_SCHEDULING = 2;
  JANK_PREDICTION_ERROR = 4;
  JANK_DISPLAY_HAL = 8;
  JANK_SF_CPU_DEADLINE_MISSED = 16;
  JANK_SF_GPU_DEADLINE_MISSED = 32;
  JANK_APP_DEADLINE_MISSED = 64;
  JANK_BUFFER_STUFFING = 128;
  JANK_UNKNOWN = 256;
  JANK_SF_STUFFING = 512;

class PresentType:
  PRESENT_UNSPECIFIED = 0;
  PRESENT_ON_TIME = 1;
  PRESENT_LATE = 2;
  PRESENT_EARLY = 3;
  PRESENT_DROPPED = 4;
  PRESENT_UNKNOWN = 5;

class PredictionType:
  PREDICTION_UNSPECIFIED = 0;
  PREDICTION_VALID = 1;
  PREDICTION_EXPIRED = 2;
  PREDICTION_UNKNOWN = 3;

trace = synth_common.create_trace()

trace.add_packet(ts=5)
trace.add_process(1001, 0, "process1")
trace.add_process(1002, 0, "process2")
trace.add_process(1003, 0, "process3")

trace.add_actual_surface_frame_start_event(ts=21, cookie=6, token=1, display_frame_token=4, pid=1002, layer_name="Layer1", present_type=PresentType.PRESENT_ON_TIME, on_time_finish=1, gpu_composition=0, jank_type=JankType.JANK_NONE, prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=37, cookie=6)

trace.add_actual_surface_frame_start_event(ts=31, cookie=7, token=1, display_frame_token=4, pid=1002, layer_name="Layer1", present_type=PresentType.PRESENT_ON_TIME, on_time_finish=1, gpu_composition=0, jank_type=JankType.JANK_APP_DEADLINE_MISSED, prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=47, cookie=7)


# DisplayFrame with a janky SurfaceFrame
trace.add_actual_surface_frame_start_event(ts=41, cookie=10, token=5, display_frame_token=6, pid=1001, layer_name="Layer1", present_type=PresentType.PRESENT_LATE, on_time_finish=0, gpu_composition=0, jank_type=JankType.JANK_APP_DEADLINE_MISSED, prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=74, cookie=10)
trace.add_actual_surface_frame_start_event(ts=41, cookie=11, token=5, display_frame_token=6, pid=1001, layer_name="Layer1", present_type=PresentType.PRESENT_LATE, on_time_finish=0, gpu_composition=0, jank_type=JankType.JANK_APP_DEADLINE_MISSED|JankType.JANK_BUFFER_STUFFING, prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=75, cookie=11)

trace.add_actual_surface_frame_start_event(ts=81, cookie=15, token=8, display_frame_token=9, pid=1003, layer_name="Layer1", present_type=PresentType.PRESENT_LATE, on_time_finish=0, gpu_composition=0, jank_type=JankType.JANK_SF_CPU_DEADLINE_MISSED, prediction_type=PredictionType.PREDICTION_VALID)
trace.add_frame_end_event(ts=85, cookie=15)
sys.stdout.buffer.write(trace.trace.SerializeToString())
