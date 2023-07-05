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


class PowerPowerRails(TestSuite):

  def test_power_rails_power_rails(self):
    return DiffTestBlueprint(
        trace=DataPath('power_rails.pb'),
        query="""
        SELECT name, AVG(value), COUNT(*)
        FROM counters
        WHERE name GLOB "power.*"
        GROUP BY name
        LIMIT 20;
        """,
        out=Csv("""
        "name","AVG(value)","COUNT(*)"
        "power.PPVAR_VPH_PWR_ABH_uws",7390700.360656,61
        "power.PPVAR_VPH_PWR_OLED_uws",202362991.655738,61
        """))

  def test_power_rails_event_power_rails_custom_clock(self):
    return DiffTestBlueprint(
        trace=Path('power_rails_custom_clock.textproto'),
        query="""
        SELECT ts, value
        FROM counters
        WHERE name GLOB "power.*"
        LIMIT 20;
        """,
        out=Csv("""
        "ts","value"
        104000000,333.000000
        106000000,666.000000
        106000000,999.000000
        109000000,0.000000
        """))

  def test_power_rails_timestamp_sort(self):
    return DiffTestBlueprint(
        trace=Path('power_rails.textproto'),
        query="""
        SELECT ts, extract_arg(arg_set_id,'packet_ts') as packet_ts, value, t.name AS name
        FROM counter c JOIN counter_track t ON t.id = c.track_id
        ORDER BY ts
        LIMIT 20;
        """,
        out=Csv("""
        "ts","packet_ts","value","name"
        3000000,3000003,333.000000,"power.test_rail_uws"
        3000000,3000005,0.000000,"power.test_rail_uws"
        3000004,"[NULL]",1000.000000,"Testing"
        3000005,3000005,999.000000,"power.test_rail2_uws"
        5000000,3000005,666.000000,"power.test_rail_uws"
        """))

  def test_power_rails_well_known_power_rails(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          power_rails {
            rail_descriptor {
              index: 4
              rail_name: "S3M_VDD_CPUCL1"
              subsys_name: "cpu"
              sampling_rate: 1023
            }
          }
        }
        packet {
          timestamp: 3000003
          power_rails {
            energy_data {
              index: 4
              timestamp_ms: 3
              energy: 333
            }
          }
        }
        packet {
          timestamp: 3000005
          power_rails {
            rail_descriptor {
              index: 3
              rail_name: "S2S_VDD_G3D"
              subsys_name: "gpu"
              sampling_rate: 1022
            }
            energy_data {
              index: 4
              timestamp_ms: 5
              energy: 666
            }
            energy_data {
              index: 3
              energy: 999
            }
            energy_data {
              index: 4
              timestamp_ms: 3
              energy: 0
            }
          }
        }
        """),
        query="""
        SELECT name, AVG(value), COUNT(*)
        FROM counters
        WHERE name GLOB "power.*"
        GROUP BY name
        LIMIT 20;
        """,
        out=Csv("""
        "name","AVG(value)","COUNT(*)"
        "power.rails.cpu.mid",333.000000,3
        "power.rails.gpu",999.000000,1
        """))
