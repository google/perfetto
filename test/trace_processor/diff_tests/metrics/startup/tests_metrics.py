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


class StartupMetrics(TestSuite):

  def test_android_startup(self):
    return DiffTestBlueprint(
        trace=Path('android_startup.py'),
        query=Metric('android_startup'),
        out=Path('android_startup.out'))

  def test_android_startup_slow(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_slow.py'),
        query=Metric('android_startup'),
        out=Path('android_startup_slow.out'))

  def test_android_startup_minsdk33(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_minsdk33.py'),
        query=Metric('android_startup'),
        out=Path('android_startup_minsdk33.out'))

  def test_android_startup_breakdown(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_breakdown.py'),
        query=Metric('android_startup'),
        out=Path('android_startup_breakdown.out'))

  def test_android_startup_breakdown_slow(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_breakdown_slow.py'),
        query=Metric('android_startup'),
        out=Path('android_startup_breakdown_slow.out'))

  def test_android_startup_process_track(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_process_track.py'),
        query=Metric('android_startup'),
        out=Path('android_startup_process_track.out'))

  def test_android_startup_attribution(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_attribution.py'),
        query=Metric('android_startup'),
        out=Path('android_startup_attribution.out'))

  def test_android_startup_attribution_slow(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_attribution_slow.py'),
        query=Metric('android_startup'),
        out=Path('android_startup_attribution_slow.out'))

  # Other metrics associated with startup.
  def test_android_batt_counters(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_battery.py'),
        query=Metric('android_batt'),
        out=TextProto(r"""
        android_batt{
           battery_counters{
              timestamp_ns: 20
              charge_counter_uah: 52
              capacity_percent: 0.2
              current_ua: 10
              current_avg_ua: 12
           }
           battery_counters {
              timestamp_ns: 52
              charge_counter_uah: 32
              capacity_percent: 0.8
              current_ua: 8
              current_avg_ua: 93
           }
           battery_counters {
              timestamp_ns: 80
              charge_counter_uah: 15
              capacity_percent: 0.5
              current_ua: 9
              current_avg_ua: 5
           }
           battery_counters {
              timestamp_ns: 92
              charge_counter_uah: 21
              capacity_percent: 0.3
              current_avg_ua: 25
           }
        }
        """))

  def test_android_startup_cpu(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_cpu.py'),
        query=Metric('android_cpu'),
        out=Path('android_startup_cpu.out'))

  def test_android_startup_powrails(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_powrails.py'),
        query=Metric('android_powrails'),
        out=Path('android_startup_powrails.out'))
