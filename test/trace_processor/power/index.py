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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Power(DiffTestModule):

  def test_power_rails_power_rails(self):
    return DiffTestBlueprint(
        trace=Path('../../data/power_rails.pb'),
        query=Path('power_rails_test.sql'),
        out=Path('power_rails_power_rails.out'))

  def test_power_rails_event_power_rails_custom_clock(self):
    return DiffTestBlueprint(
        trace=Path('power_rails_custom_clock.textproto'),
        query=Path('power_rails_event_test.sql'),
        out=Path('power_rails_event_power_rails_custom_clock.out'))

  def test_power_rails_timestamp_sort(self):
    return DiffTestBlueprint(
        trace=Path('power_rails.textproto'),
        query=Path('power_rails_timestamp_sort_test.sql'),
        out=Path('power_rails_timestamp_sort.out'))

  def test_power_rails_well_known_power_rails(self):
    return DiffTestBlueprint(
        trace=Path('power_rails_well_known.textproto'),
        query=Path('power_rails_test.sql'),
        out=Path('power_rails_well_known_power_rails.out'))

  def test_dvfs_metric(self):
    return DiffTestBlueprint(
        trace=Path('dvfs_metric.textproto'),
        query=Path('android_dvfs'),
        out=Path('dvfs_metric.out'))

  def test_wakesource_wakesource(self):
    return DiffTestBlueprint(
        trace=Path('wakesource.textproto'),
        query=Path('wakesource_test.sql'),
        out=Path('wakesource_wakesource.out'))

  def test_suspend_resume(self):
    return DiffTestBlueprint(
        trace=Path('suspend_resume.textproto'),
        query=Path('suspend_resume_test.sql'),
        out=Path('suspend_resume.out'))

  def test_suspend_period(self):
    return DiffTestBlueprint(
        trace=Path('suspend_period.textproto'),
        query=Path('android_batt'),
        out=Path('suspend_period.out'))

  def test_energy_breakdown_table_test(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown.textproto'),
        query=Path('energy_breakdown_table_test.sql'),
        out=Path('energy_breakdown_table_test.out'))

  def test_energy_breakdown_event_test(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown.textproto'),
        query=Path('energy_breakdown_event_test.sql'),
        out=Path('energy_breakdown_event_test.out'))

  def test_energy_breakdown_uid_table_test(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown_uid.textproto'),
        query=Path('energy_breakdown_uid_table_test.sql'),
        out=Path('energy_breakdown_uid_table_test.out'))

  def test_energy_breakdown_uid_event_test(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown_uid.textproto'),
        query=Path('energy_breakdown_uid_event_test.sql'),
        out=Path('energy_breakdown_uid_event_test.out'))

  def test_energy_per_uid_table_test(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown_uid.textproto'),
        query=Path('energy_per_uid_table_test.sql'),
        out=Path('energy_per_uid_table_test.out'))

  def test_cpu_counters_p_state_test(self):
    return DiffTestBlueprint(
        trace=Path('../../data/cpu_counters.pb'),
        query=Path('p_state_test.sql'),
        out=Path('cpu_counters_p_state_test.out'))

  def test_cpu_powerups_test(self):
    return DiffTestBlueprint(
        trace=Path('../../data/cpu_powerups_1.pb'),
        query=Path('cpu_powerups_test.sql'),
        out=Path('cpu_powerups_test.out'))
