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


class LinuxSysfsPower(TestSuite):

  # Test basic battery counters.
  def test_counters(self):
    return DiffTestBlueprint(
        trace=TextProto("""
        packet {
          timestamp: 3000000
          battery {
            charge_counter_uah: 3005000
            capacity_percent: 100.000000
            current_ua: 0
          }
        }
        """),
        query="""
        SELECT * FROM (
          (SELECT AVG(value) AS capacity_percent FROM counters
           WHERE name='batt.capacity_pct'),
          (SELECT AVG(value) AS charge_uah FROM counters
           WHERE name='batt.charge_uah'),
          (SELECT AVG(value) AS current_ua FROM counters
           WHERE name='batt.current_ua')
        );
        """,
        out=Csv("""
        "capacity_percent","charge_uah","current_ua"
        100.000000,3005000.000000,0.000000
        """))

  # Test multiple batteries.
  def test_multiple_batteries(self):
    return DiffTestBlueprint(
        trace=TextProto("""
        packet {
          timestamp: 3000000
          battery {
            charge_counter_uah: 3005000
            capacity_percent: 100.000000
            current_ua: 0
            name: "BAT0"
          }
        }
        packet {
          timestamp: 3000000
          battery {
            capacity_percent: 90.000000
            name: "BAT1"
          }
        }
        """),
        query="""
        SELECT name, value FROM counters WHERE name like "batt.%" ORDER BY name
        """,
        out=Csv("""
        "name","value"
        "batt.BAT0.capacity_pct",100.000000
        "batt.BAT0.charge_uah",3005000.000000
        "batt.BAT0.current_ua",0.000000
        "batt.BAT1.capacity_pct",90.000000
        """))

  # Test convertion to charge counter from energy and voltage.
  def test_charge_from_energy_and_voltage(self):
    return DiffTestBlueprint(
        trace=TextProto("""
        packet {
          timestamp: 3000000
          battery {
            energy_counter_uwh: 56680000
            voltage_uv: 17356000
          }
        }
        packet {
          timestamp: 4000000
          battery {
            energy_counter_uwh: 56600000
            voltage_uv: 17356000
          }
        }
        """),
        query="""
        SELECT value
        FROM counters
        WHERE name = "batt.charge_uah"
        """,
        out=Csv("""
        "value"
        3265729.000000
        3261120.000000
        """))

  # Test convertion to charge counter from energy and voltage: bad voltage
  # value.
  def test_charge_from_energy_and_bad_voltage(self):
    return DiffTestBlueprint(
        trace=TextProto("""
        packet {
          timestamp: 3000000
          battery {
            energy_counter_uwh: 56680000
            voltage_uv: 0
          }
        }
        """),
        query="""
        SELECT value
        FROM counters
        WHERE name = "batt.charge_uah"
        """,
        out=Csv("""
        "value"
        """))

  # Test calculating power counter from current and voltage.
  def test_power_from_current_and_voltage(self):
    return DiffTestBlueprint(
        trace=TextProto("""
        packet {
          timestamp: 3000000
          battery {
            current_ua: 710000
            voltage_uv: 11900000
          }
        }
        packet {
          timestamp: 4000000
          battery {
            current_ua: 510000
            voltage_uv: 12000000
          }
        }
        """),
        query="""
        SELECT value
        FROM counters
        WHERE name = "batt.power_mw"
        """,
        out=Csv("""
        "value"
        8449.000000
        6120.000000
        """))
