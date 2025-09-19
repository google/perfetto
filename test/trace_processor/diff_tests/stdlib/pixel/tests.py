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

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class PixelStdlib(TestSuite):

  def test_android_camera_frames(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet { ftrace_events {
          cpu: 0
          previous_bundle_end_timestamp: 2000
          event {
            timestamp: 2200
            pid: 42
            print { buf: "B|42|cam1_filter:output (frame 123)\n" }
          }
          event {
            timestamp: 2700
            pid: 42
            print { buf: "E|42\n" }
          }
        }}
        """),
        query="""
        INCLUDE PERFETTO MODULE pixel.camera;

        SELECT
          ts,
          node,
          port_group,
          frame_number,
          cam_id,
          dur
        FROM pixel_camera_frames
        ORDER BY ts
        """,
        out=Csv("""
        "ts","node","port_group","frame_number","cam_id","dur"
        2200,"filter","output",123,1,500
        """))
