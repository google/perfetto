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


class Power(TestSuite):
  # Power states
  def test_cpu_counters_p_state(self):
    return DiffTestBlueprint(
        trace=DataPath('cpu_counters.pb'),
        query="""
        SELECT RUN_METRIC("android/p_state.sql");

        SELECT * FROM P_STATE_OVER_INTERVAL(2579596465618, 2579606465618);
        """,
        out=Path('cpu_counters_p_state_test.out'))

  # CPU power ups
  def test_cpu_powerups(self):
    return DiffTestBlueprint(
        trace=DataPath('cpu_powerups_1.pb'),
        query="""
        INCLUDE PERFETTO MODULE chrome.cpu_powerups;
        SELECT * FROM chrome_cpu_power_first_toplevel_slice_after_powerup;
        """,
        out=Csv("""
        "slice_id","previous_power_state"
        424,2
        703,2
        708,2
        """))
