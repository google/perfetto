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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class PowerPowerRails(TestSuite):

  def test_android_power_rails_counters(self):
    return DiffTestBlueprint(
        trace=DataPath('power_rails.pb'),
        query="""
        INCLUDE PERFETTO MODULE android.power_rails;
        SELECT
        power_rail_name, AVG(value), COUNT(*)
        FROM android_power_rails_counters
        GROUP BY 1
        LIMIT 20;
      """,
        out=Csv("""
        "power_rail_name","AVG(value)","COUNT(*)"
        "power.PPVAR_VPH_PWR_ABH_uws",7388261.216667,60
        "power.PPVAR_VPH_PWR_OLED_uws",202362991.655738,61
        """))

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

  def test_power_rails_session_uuid(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          power_rails {
            session_uuid: 100
            rail_descriptor {
              index: 0
              rail_name: "CPU_RAIL"
              subsys_name: "cpu"
              sampling_rate: 1000
            }
          }
        }
        packet {
          power_rails {
            session_uuid: 200
            rail_descriptor {
              index: 0
              rail_name: "GPU_RAIL"
              subsys_name: "gpu"
              sampling_rate: 2000
            }
          }
        }
        packet {
          timestamp: 1000000
          power_rails {
            session_uuid: 100
            energy_data {
              index: 0
              timestamp_ms: 1
              energy: 500
            }
          }
        }
        packet {
          timestamp: 2000000
          power_rails {
            session_uuid: 200
            energy_data {
              index: 0
              timestamp_ms: 2
              energy: 750
            }
          }
        }
        packet {
          timestamp: 3000000
          power_rails {
            session_uuid: 100
            energy_data {
              index: 0
              timestamp_ms: 3
              energy: 1000
            }
          }
        }
        packet {
          timestamp: 4000000
          power_rails {
            session_uuid: 200
            energy_data {
              index: 0
              timestamp_ms: 4
              energy: 1250
            }
          }
        }
        """),
        query="""
        SELECT
          name,
          ts,
          value
        FROM counters
        WHERE name GLOB "power.*"
        ORDER BY name, ts;
        """,
        out=Csv("""
        "name","ts","value"
        "power.CPU_RAIL_uws",1000000,500.000000
        "power.CPU_RAIL_uws",3000000,1000.000000
        "power.GPU_RAIL_uws",2000000,750.000000
        "power.GPU_RAIL_uws",4000000,1250.000000
        """))

  def test_power_rails_session_uuid_same_index_same_name(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          power_rails {
            session_uuid: 100
            rail_descriptor {
              index: 0
              rail_name: "SHARED_RAIL"
              sampling_rate: 1000
            }
          }
        }
        packet {
          power_rails {
            session_uuid: 200
            rail_descriptor {
              index: 0
              rail_name: "SHARED_RAIL"
              sampling_rate: 2000
            }
          }
        }
        packet {
          timestamp: 1000000
          power_rails {
            session_uuid: 100
            energy_data {
              index: 0
              timestamp_ms: 1
              energy: 100
            }
          }
        }
        packet {
          timestamp: 2000000
          power_rails {
            session_uuid: 200
            energy_data {
              index: 0
              timestamp_ms: 2
              energy: 200
            }
          }
        }
        packet {
          timestamp: 3000000
          power_rails {
            session_uuid: 100
            energy_data {
              index: 0
              timestamp_ms: 3
              energy: 300
            }
          }
        }
        packet {
          timestamp: 4000000
          power_rails {
            session_uuid: 200
            energy_data {
              index: 0
              timestamp_ms: 4
              energy: 400
            }
          }
        }
        """),
        query="""
        SELECT
          t.name,
          c.ts,
          c.value,
          extract_arg(t.dimension_arg_set_id, 'session_uuid') as session_uuid
        FROM counter c JOIN counter_track t ON t.id = c.track_id
        WHERE t.name GLOB "power.*"
        ORDER BY session_uuid, ts;
        """,
        out=Csv("""
        "name","ts","value","session_uuid"
        "power.SHARED_RAIL_uws",1000000,100.000000,100
        "power.SHARED_RAIL_uws",3000000,300.000000,100
        "power.SHARED_RAIL_uws",2000000,200.000000,200
        "power.SHARED_RAIL_uws",4000000,400.000000,200
        """))

  def test_android_power_rails_metadata(self):
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
          power_rails {
            rail_descriptor {
              index: 3
              rail_name: "S2S_VDD_G3D"
              subsys_name: "gpu"
              sampling_rate: 1022
            }
          }
        }
        packet {
          power_rails {
            rail_descriptor {
              index: 5
              rail_name: "L14S_ALIVE"
              subsys_name: "system"
              sampling_rate: 1024
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.power_rails;
        SELECT
          power_rail_name,
          raw_power_rail_name,
          friendly_name,
          subsystem_name
        FROM android_power_rails_metadata
        ORDER BY power_rail_name;
        """,
        out=Csv("""
        "power_rail_name","raw_power_rail_name","friendly_name","subsystem_name"
        "power.L14S_ALIVE_uws","L14S_ALIVE","[NULL]","system"
        "power.rails.cpu.mid","S3M_VDD_CPUCL1","cpu.mid","cpu"
        "power.rails.gpu","S2S_VDD_G3D","gpu","gpu"
        """))

  def test_power_rails_multi_device(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          power_rails {
            rail_descriptor {
              index: 0
              rail_name: "SHARED_RAIL"
              subsys_name: "cpu"
              sampling_rate: 1000
            }
          }
        }
        packet {
          timestamp: 1000000
          power_rails {
            energy_data {
              index: 0
              energy: 100
            }
          }
        }
        packet {
          timestamp: 3000000
          power_rails {
            energy_data {
              index: 0
              energy: 300
            }
          }
        }
        packet {
          machine_id: 1
          remote_clock_sync {
            synced_clocks {
              client_clocks {
                clocks {
                  clock_id: 6
                  timestamp: 10000
                }
              }
              host_clocks {
                clocks {
                  clock_id: 6
                  timestamp: 1000000
                }
              }
            }
            synced_clocks {
              client_clocks {
                clocks {
                  clock_id: 6
                  timestamp: 10000
                }
              }
              host_clocks {
                clocks {
                  clock_id: 6
                  timestamp: 1000000
                }
              }
            }
          }
        }
        packet {
          machine_id: 1
          power_rails {
            rail_descriptor {
              index: 0
              rail_name: "SHARED_RAIL"
              subsys_name: "gpu"
              sampling_rate: 2000
            }
          }
        }
        packet {
          machine_id: 1
          timestamp: 2000000
          timestamp_clock_id: 6  # BUILTIN_CLOCK_BOOTTIME
          power_rails {
            energy_data {
              index: 0
              energy: 200
            }
          }
        }
        packet {
          machine_id: 1
          timestamp: 4000000
          timestamp_clock_id: 6  # BUILTIN_CLOCK_BOOTTIME
          power_rails {
            energy_data {
              index: 0
              timestamp_ms: 4
              energy: 400
            }
          }
        }
        """),
        query="""
        SELECT
          t.name,
          c.ts,
          c.value,
          t.machine_id
        FROM counter c
        JOIN counter_track t ON t.id = c.track_id
        WHERE t.name GLOB "power.*"
        ORDER BY t.name, t.machine_id, c.ts;
        """,
        out=Csv("""
        "name","ts","value","machine_id"
        "power.SHARED_RAIL_uws",1000000,100.000000,"[NULL]"
        "power.SHARED_RAIL_uws",3000000,300.000000,"[NULL]"
        "power.SHARED_RAIL_uws",2990000,200.000000,1
        "power.SHARED_RAIL_uws",4990000,400.000000,1
        """))
