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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Battery(TestSuite):

  def test_android_battery_charge(self):
    return DiffTestBlueprint(
        trace=TextProto("""
        packet {
          timestamp: 3000000
          battery {
            charge_counter_uah: 3005000
            capacity_percent: 100.000000
            current_ua: 710000
            current_avg_ua: 750000
            voltage_uv: 11900000
            energy_counter_uwh: 50000000
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.battery;
        SELECT * FROM android_battery_charge;
        """,
        out=Csv("""
        "ts","current_avg_ua","capacity_percent","charge_uah","current_ua","voltage_uv","energy_counter_uwh","power_mw"
        3000000,750000.000000,100.000000,3005000.000000,710000.000000,11900000.000000,"[NULL]",8449.000000
        """))