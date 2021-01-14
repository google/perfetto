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
  JANK_UNSPECIFIED = 0;
  JANK_NONE = 1;
  JANK_SF_SCHEDULING = 2;
  JANK_PREDICTION_ERROR = 3;
  JANK_DISPLAY_HAL = 4;
  JANK_SF_DEADLINE_MISSED = 5;
  JANK_APP_DEADLINE_MISSED = 6;
  JANK_BUFFER_STUFFING = 7;
  JANK_UNKNOWN = 8;

class PresentType:
  PRESENT_UNSPECIFIED = 0;
  PRESENT_ON_TIME = 1;
  PRESENT_LATE = 2;
  PRESENT_EARLY = 3;
  PRESENT_DROPPED = 4;

trace = synth_common.create_trace()

# DisplayFrame without a SurfaceFrame
trace.add_expected_display_frame_start_event(ts=20, cookie=1, token=2, pid=666)
trace.add_frame_end_event(ts=26, cookie=1)
trace.add_actual_display_frame_start_event(ts=20, cookie=2, token=2, pid=666, present_type=PresentType.PRESENT_ON_TIME, on_time_finish=1, gpu_composition=0, jank_type=JankType.JANK_NONE)
trace.add_frame_end_event(ts=26, cookie=2)

# DisplayFrame with a SurfaceFrame
trace.add_expected_display_frame_start_event(ts=40, cookie=3, token=4, pid=666)
trace.add_frame_end_event(ts=46, cookie=3)
trace.add_actual_display_frame_start_event(ts=42, cookie=4, token=4, pid=666, present_type=PresentType.PRESENT_ON_TIME, on_time_finish=1, gpu_composition=0, jank_type=JankType.JANK_NONE)
trace.add_frame_end_event(ts=47, cookie=4)
trace.add_expected_surface_frame_start_event(ts=21, cookie=5, token=1, display_frame_token=4, pid=1000, layer_name="Layer1")
trace.add_frame_end_event(ts=36, cookie=5)
trace.add_actual_surface_frame_start_event(ts=21, cookie=6, token=1, display_frame_token=4, pid=1000, layer_name="Layer1", present_type=PresentType.PRESENT_ON_TIME, on_time_finish=1, gpu_composition=0, jank_type=JankType.JANK_NONE)
trace.add_frame_end_event(ts=37, cookie=6)


# DisplayFrame with a janky SurfaceFrame
trace.add_expected_display_frame_start_event(ts=80, cookie=7, token=6, pid=666)
trace.add_frame_end_event(ts=86, cookie=7)
trace.add_actual_display_frame_start_event(ts=81, cookie=8, token=6, pid=666, present_type=PresentType.PRESENT_ON_TIME, on_time_finish=1, gpu_composition=0, jank_type=JankType.JANK_NONE)
trace.add_frame_end_event(ts=88, cookie=8)
trace.add_expected_surface_frame_start_event(ts=41, cookie=9, token=5, display_frame_token=6, pid=1000, layer_name="Layer1")
trace.add_frame_end_event(ts=56, cookie=9)
trace.add_actual_surface_frame_start_event(ts=41, cookie=10, token=5, display_frame_token=6, pid=1000, layer_name="Layer1", present_type=PresentType.PRESENT_LATE, on_time_finish=0, gpu_composition=0, jank_type=JankType.JANK_APP_DEADLINE_MISSED)
trace.add_frame_end_event(ts=74, cookie=10)


# Janky DisplayFrame with a SurfaceFrame
trace.add_expected_display_frame_start_event(ts=120, cookie=11, token=8, pid=666)
trace.add_frame_end_event(ts=126, cookie=11)
trace.add_actual_display_frame_start_event(ts=108, cookie=12, token=8, pid=666, present_type=PresentType.PRESENT_EARLY, on_time_finish=1, gpu_composition=0, jank_type=JankType.JANK_SF_SCHEDULING)
trace.add_frame_end_event(ts=112, cookie=12)
trace.add_expected_surface_frame_start_event(ts=90, cookie=13, token=7, display_frame_token=8, pid=1000, layer_name="Layer1")
trace.add_frame_end_event(ts=106, cookie=13)
trace.add_actual_surface_frame_start_event(ts=90, cookie=14, token=7, display_frame_token=8, pid=1000, layer_name="Layer1", present_type=PresentType.PRESENT_EARLY, on_time_finish=1, gpu_composition=0, jank_type=JankType.JANK_SF_SCHEDULING)
trace.add_frame_end_event(ts=106, cookie=14)

sys.stdout.buffer.write(trace.trace.SerializeToString())
