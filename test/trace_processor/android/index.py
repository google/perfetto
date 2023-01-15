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

from python.generators.diff_tests.testing import Path, Metric
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Android(DiffTestModule):

  def test_game_intervention_list_test(self):
    return DiffTestBlueprint(
        trace=Path('game_intervention_list_test.textproto'),
        query=Path('game_intervention_list_test.sql'),
        out=Path('game_intervention_list_test.out'))

  def test_android_system_property_counter(self):
    return DiffTestBlueprint(
        trace=Path('android_system_property.textproto'),
        query=Path('android_system_property_counter_test.sql'),
        out=Path('android_system_property_counter.out'))

  def test_android_system_property_slice(self):
    return DiffTestBlueprint(
        trace=Path('android_system_property.textproto'),
        query=Path('android_system_property_slice_test.sql'),
        out=Path('android_system_property_slice.out'))

  def test_android_bugreport_logs_test(self):
    return DiffTestBlueprint(
        trace=Path('../../data/bugreport-crosshatch-SPB5.zip'),
        query=Path('android_bugreport_logs_test.sql'),
        out=Path('android_bugreport_logs_test.out'))

  def test_android_bugreport_dumpstate_test(self):
    return DiffTestBlueprint(
        trace=Path('../../data/bugreport-crosshatch-SPB5.zip'),
        query=Path('android_bugreport_dumpstate_test.sql'),
        out=Path('android_bugreport_dumpstate_test.out'))

  def test_android_bugreport_dumpsys_test(self):
    return DiffTestBlueprint(
        trace=Path('../../data/bugreport-crosshatch-SPB5.zip'),
        query=Path('android_bugreport_dumpsys_test.sql'),
        out=Path('android_bugreport_dumpsys_test.out'))
