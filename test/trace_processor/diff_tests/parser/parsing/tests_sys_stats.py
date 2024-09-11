#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite

class ParsingSysStats(TestSuite):

  def test_cpuidle_stats(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          sys_stats {
            cpuidle_state {
              cpu_id: 0
              cpuidle_state_entry {
                state: "C8"
                duration_us: 486626084
              }
            }
          }
          timestamp: 71625871363623
          trusted_packet_sequence_id: 2
        }
        packet {
          sys_stats {
            cpuidle_state {
              cpu_id: 0
              cpuidle_state_entry {
                state: "C8"
                duration_us: 486636254
              }
            }
          }
          timestamp: 71626000387166
          trusted_packet_sequence_id: 2
        }
        """),
        query="""
        SELECT ts, cct.name, value, cct.cpu
        FROM counter c
        JOIN cpu_counter_track cct on c.track_id = cct.id
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","name","value","cpu"
        71625871363623,"cpuidle.C8",486626084.000000,0
        71626000387166,"cpuidle.C8",486636254.000000,0
        """))

  def test_thermal_zones(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          sys_stats {
            thermal_zone {
              name: "thermal_zone0"
              temp: 29
              type: "x86_pkg_temp"
            }
          }
          timestamp: 71625871363623
          trusted_packet_sequence_id: 2
        }
        packet {
          sys_stats {
            thermal_zone {
              name: "thermal_zone0"
              temp: 31
              type: "x86_pkg_temp"
            }
          }
          timestamp: 71626000387166
          trusted_packet_sequence_id: 2
        }
        """),
        query="""
        SELECT c.ts,
               t.name,
               c.value
        FROM counter_track t
        JOIN counter c ON t.id = c.track_id
        """,
        out=Csv("""
        "ts","name","value"
        71625871363623,"x86_pkg_temp",29.000000
        71626000387166,"x86_pkg_temp",31.000000
        """))

  def test_gpufreq(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
    packet {
      sys_stats {
        gpufreq_mhz: 300
      }
      timestamp: 115835063108
      trusted_packet_sequence_id: 2
    }
    packet {
      sys_stats {
        gpufreq_mhz: 350
      }
      timestamp: 115900182490
      trusted_packet_sequence_id: 2
    }
    """),
        query="""
    SELECT c.ts,
            t.name,
            c.value
    FROM counter_track t
    JOIN counter c ON t.id = c.track_id
    """,
        out=Csv("""
    "ts","name","value"
    115835063108,"gpufreq",300.000000
    115900182490,"gpufreq",350.000000
    """))
