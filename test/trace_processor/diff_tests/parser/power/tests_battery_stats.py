#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class BatteryStats(TestSuite):

  def test_battery_stats_tracks(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          android_system_property {
            values {
              name: "debug.tracing.battery_stats.screen"
              value: "1"
            }
          }
          timestamp: 1000000
        }
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 2000000
              pid: 1000
              print {
                buf: "C|1000|battery_stats.screen|0"
              }
            }
          }
        }
        packet {
          android_system_property {
            values {
              name: "debug.tracing.battery_stats.wifi"
              value: "1"
            }
          }
          timestamp: 3000000
        }
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 4000000
              pid: 1000
              print {
                buf: "C|1000|battery_stats.wifi|0"
              }
            }
          }
        }
        """),
        query="""
        SELECT
          t.name AS track_name,
          c.ts,
          c.value,
          t.type AS track_type
        FROM counter c
        JOIN counter_track t ON c.track_id = t.id
        ORDER BY t.name, c.ts;
        """,
        out=Csv("""
        "track_name","ts","value","track_type"
        "battery_stats.screen",1000000,1.000000,"battery_stats"
        "battery_stats.screen",2000000,0.000000,"battery_stats"
        "battery_stats.wifi",3000000,1.000000,"battery_stats"
        "battery_stats.wifi",4000000,0.000000,"battery_stats"
        """))
