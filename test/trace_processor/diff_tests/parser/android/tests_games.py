#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License a
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class AndroidGames(TestSuite):
  # Ensure Android game intervention list are parsed correctly
  def test_game_intervention_list(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
            android_game_intervention_list {
              parse_error: false
              read_error: false
              game_packages {
                name: "com.test.game1"
                uid: 1001
                current_mode: 1
                game_mode_info {
                  mode: 1
                  use_angle: true
                  resolution_downscale: 1.0
                  fps: 0.0
                }
                game_mode_info {
                  mode: 2
                  use_angle: false
                  resolution_downscale: 1.0
                  fps: 60.0
                }
                game_mode_info {
                  mode: 3
                  use_angle: true
                  resolution_downscale: 0.75
                  fps: 120.0
                }
              }
              game_packages {
                name: "com.test.game2"
                uid: 1002
                current_mode: 3
                game_mode_info {
                  mode: 1
                  use_angle: false
                  resolution_downscale: 1.0
                  fps: 0.0
                }
                game_mode_info {
                  mode: 3
                  use_angle: false
                  resolution_downscale:  0.95
                  fps: 45.0
                }
              }
            }
        }
        """),
        query="""
        SELECT
          package_name,
          uid,
          current_mode,
          standard_mode_supported,
          standard_mode_downscale,
          standard_mode_use_angle,
          standard_mode_fps,
          perf_mode_supported,
          perf_mode_downscale,
          perf_mode_use_angle,
          perf_mode_fps,
          battery_mode_supported,
          battery_mode_downscale,
          battery_mode_use_angle,
          battery_mode_fps
        FROM android_game_intervention_list
        ORDER BY package_name;
        """,
        out=Path('game_intervention_list_test.out'))
