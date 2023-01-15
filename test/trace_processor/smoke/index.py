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


class DiffTestModule_Smoke(DiffTestModule):

  def test_sfgate_smoke(self):
    return DiffTestBlueprint(
        trace=Path('../../data/sfgate.json'),
        query=Path('../common/smoke_test.sql'),
        out=Path('sfgate_smoke.out'))

  def test_sfgate_smoke_slices(self):
    return DiffTestBlueprint(
        trace=Path('../../data/sfgate.json'),
        query=Path('../common/smoke_slices_test.sql'),
        out=Path('sfgate_smoke_slices.out'))

  def test_android_sched_and_ps_smoke(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query=Path('../common/smoke_test.sql'),
        out=Path('android_sched_and_ps_smoke.out'))

  def test_compressed_smoke(self):
    return DiffTestBlueprint(
        trace=Path('../../data/compressed.pb'),
        query=Path('../common/smoke_test.sql'),
        out=Path('compressed_smoke.out'))

  def test_synth_1_smoke(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('../common/smoke_test.sql'),
        out=Path('synth_1_smoke.out'))

  def test_thread_cpu_time_example_android_trace_30s(self):
    return DiffTestBlueprint(
        trace=Path('../../data/example_android_trace_30s.pb'),
        query=Path('thread_cpu_time_test.sql'),
        out=Path('thread_cpu_time_example_android_trace_30s.out'))

  def test_proxy_power(self):
    return DiffTestBlueprint(
        trace=Path('../../data/cpu_counters.pb'),
        query=Path('proxy_power_test.sql'),
        out=Path('proxy_power.out'))
