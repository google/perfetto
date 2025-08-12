#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

from python.generators.diff_tests.testing import DataPath, Metric
from python.generators.diff_tests.testing import TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ChromeScrollJankMetrics(TestSuite):

  def test_chrome_scroll_jank_v3(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query=Metric('chrome_scroll_jank_v3'),
        out=TextProto(r"""
        [perfetto.protos.chrome_scroll_jank_v3] {
          trace_num_frames: 364
          trace_num_janky_frames: 6
          trace_scroll_jank_percentage: 1.6483516483516483
          vsync_interval_ms: 10.318
          scrolls {
            num_frames: 119
            num_janky_frames: 1
            scroll_jank_percentage: 0.8403361344537815
            max_delay_since_last_frame: 2.153421205660012
            scroll_jank_causes {
              cause: "SubmitCompositorFrameToPresentationCompositorFrame"
              sub_cause: "StartDrawToSwapStart"
              delay_since_last_frame: 2.153421205660012
            }
          }
          scrolls {
            num_frames: 6
            num_janky_frames: 1
            scroll_jank_percentage: 16.666666666666668
            max_delay_since_last_frame: 2.155456483814693
            scroll_jank_causes {
              cause: "SubmitCompositorFrameToPresentationCompositorFrame"
              sub_cause: "StartDrawToSwapStart"
              delay_since_last_frame: 2.155456483814693
            }
          }
          scrolls {
            num_frames: 129
            num_janky_frames: 4
            scroll_jank_percentage: 3.10077519379845
            max_delay_since_last_frame: 2.1642760224849775
            scroll_jank_causes {
              cause: "SubmitCompositorFrameToPresentationCompositorFrame"
              sub_cause: "StartDrawToSwapStart"
              delay_since_last_frame: 2.1556503198294243
            }
            scroll_jank_causes {
              cause: "SubmitCompositorFrameToPresentationCompositorFrame"
              sub_cause: "BufferReadyToLatch"
              delay_since_last_frame: 2.1564256638883506
            }
            scroll_jank_causes {
              cause: "SubmitCompositorFrameToPresentationCompositorFrame"
              sub_cause: "StartDrawToSwapStart"
              delay_since_last_frame: 2.15758867997674
            }
            scroll_jank_causes {
              cause: "RendererCompositorQueueingDelay"
              delay_since_last_frame: 2.1642760224849775
            }
          }
        }
        """))
