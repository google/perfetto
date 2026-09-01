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


class ThermalExynos(TestSuite):

  def test_thermal_exynos_ftrace_event_raw_ts(self):
    """Custom tokenizers push each event twice: the ftrace_event table should
    use the raw kernel timestamp (1000000000), while the counter table should
    use the forged timestamp from the event data (5000000000)."""
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet { trusted_packet_sequence_id: 1 ftrace_events {
          cpu: 0
          event {
            timestamp: 1000000000
            pid: 42
            thermal_exynos_acpm_bulk {
              tz_id: 0
              current_temp: 35
              ctrl_temp: 40
              timestamp: 5000000000
            }
          }
        }}
        """),
        query="""
        SELECT 'ftrace_event' as source, ts
        FROM ftrace_event
        WHERE name = 'thermal_exynos_acpm_bulk'
        UNION ALL
        SELECT 'counter' as source, ts
        FROM counter
        JOIN track ON counter.track_id = track.id
        WHERE track.name = 'BIG Temperature'
        """,
        out=Csv("""
        "source","ts"
        "ftrace_event",1000000000
        "counter",5000000000
        """))
