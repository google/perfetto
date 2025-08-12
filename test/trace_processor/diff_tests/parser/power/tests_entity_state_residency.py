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


class EntityStateResidency(TestSuite):

  def test_track(self):
    return DiffTestBlueprint(
        trace=Path('entity_state_residency.textproto'),
        query="""
        select ts, ct.name, cast(c.value as int) value 
        from counter_track ct join counter c on ct.id = c.track_id 
        """,
        out=Csv("""
        "ts","name","value"
        1,"Entity residency: Bluetooth is Idle",1000
        2,"Entity residency: Bluetooth is Idle",3000
        1,"Entity residency: Bluetooth is Active",2000
        2,"Entity residency: Bluetooth is Active",4000
        1,"Entity residency: PCIe-Modem is UP",10000
        2,"Entity residency: PCIe-Modem is UP",30000
        1,"Entity residency: PCIe-Modem is DOWN",20000
        2,"Entity residency: PCIe-Modem is DOWN",40000
        """))

  def test_standard_library(self):
    return DiffTestBlueprint(
        trace=Path('entity_state_residency.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.entity_state_residency;
        select
          ts, entity_name, state_name, state_time_since_boot
        from android_entity_state_residency
        """,
        out=Csv("""
        "ts","entity_name","state_name","state_time_since_boot"
        1,"Bluetooth","Idle",1000000000
        2,"Bluetooth","Idle",3000000000
        1,"Bluetooth","Active",2000000000
        2,"Bluetooth","Active",4000000000
        1,"PCIe-Modem","UP",10000000000
        2,"PCIe-Modem","UP",30000000000
        1,"PCIe-Modem","DOWN",20000000000
        2,"PCIe-Modem","DOWN",40000000000
        """))
