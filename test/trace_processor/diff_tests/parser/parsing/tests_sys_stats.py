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
        SELECT
          ts,
          EXTRACT_ARG(t.dimension_arg_set_id, 'state') as state,
          value,
          EXTRACT_ARG(t.dimension_arg_set_id, 'cpu') as cpu
        FROM counter c
        JOIN track t on c.track_id = t.id
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","state","value","cpu"
        71625871363623,"C8",486626084.000000,0
        71626000387166,"C8",486636254.000000,0
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

  def test_disk_stats_multiple_disks(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          sys_stats {
            disk_stat {
              device_name: "sda"
              read_sectors: 100
              write_sectors: 200
              discard_sectors: 10
              flush_count: 5
              read_time_ms: 100
              write_time_ms: 200
              discard_time_ms: 50
              flush_time_ms: 25
            }
            disk_stat {
              device_name: "sdb"
              read_sectors: 300
              write_sectors: 400
              discard_sectors: 20
              flush_count: 10
              read_time_ms: 150
              write_time_ms: 250
              discard_time_ms: 75
              flush_time_ms: 30
            }
          }
          timestamp: 1000
          trusted_packet_sequence_id: 2
        }
        packet {
          sys_stats {
            disk_stat {
              device_name: "sda"
              read_sectors: 150
              write_sectors: 250
              discard_sectors: 15
              flush_count: 8
              read_time_ms: 120
              write_time_ms: 220
              discard_time_ms: 60
              flush_time_ms: 35
            }
            disk_stat {
              device_name: "sdb"
              read_sectors: 350
              write_sectors: 450
              discard_sectors: 25
              flush_count: 12
              read_time_ms: 170
              write_time_ms: 270
              discard_time_ms: 85
              flush_time_ms: 40
            }
          }
          timestamp: 2000
          trusted_packet_sequence_id: 2
        }
        """),
        query="""
        SELECT
          c.ts,
          EXTRACT_ARG(t.dimension_arg_set_id, 'device_name') as device_name,
          t.name,
          c.value,
          t.unit
        FROM counter c
        JOIN counter_track t ON c.track_id = t.id
        WHERE t.name GLOB 'diskstat.*'
        ORDER BY c.ts, device_name, t.name;
        """,
        out=Csv("""
        "ts","device_name","name","value","unit"
        2000,"sda","diskstat.[sda].discard_amount",0.002441,"MB"
        2000,"sda","diskstat.[sda].discard_throughput",0.244141,"MB/s"
        2000,"sda","diskstat.[sda].flush_amount",3.000000,"count"
        2000,"sda","diskstat.[sda].flush_time",10.000000,"ms"
        2000,"sda","diskstat.[sda].read_amount",0.024414,"MB"
        2000,"sda","diskstat.[sda].read_throughput",1.220703,"MB/s"
        2000,"sda","diskstat.[sda].write_amount",0.024414,"MB"
        2000,"sda","diskstat.[sda].write_throughput",1.220703,"MB/s"
        2000,"sdb","diskstat.[sdb].discard_amount",0.002441,"MB"
        2000,"sdb","diskstat.[sdb].discard_throughput",0.244141,"MB/s"
        2000,"sdb","diskstat.[sdb].flush_amount",2.000000,"count"
        2000,"sdb","diskstat.[sdb].flush_time",10.000000,"ms"
        2000,"sdb","diskstat.[sdb].read_amount",0.024414,"MB"
        2000,"sdb","diskstat.[sdb].read_throughput",1.220703,"MB/s"
        2000,"sdb","diskstat.[sdb].write_amount",0.024414,"MB"
        2000,"sdb","diskstat.[sdb].write_throughput",1.220703,"MB/s"
        """))
