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
              timestamp_ns: 100000000000
              charge_counter_uah: 5500000
              capacity_percent: 0.2
              current_ua: 990000
              current_avg_ua: 12
              voltage_uv: 8448000.0
           }
           battery_counters {
              timestamp_ns: 200000000000
              charge_counter_uah: 5490000
              capacity_percent: 0.8
              current_ua: 710000
              current_avg_ua: 93
              voltage_uv: 8448000.0
           }
           battery_counters {
              timestamp_ns: 300000000000
              charge_counter_uah: 5480000
              capacity_percent: 0.5
              current_ua: 510000
              current_avg_ua: 5
              voltage_uv: 8452000.0
           }
           battery_counters {
              timestamp_ns: 400000000000
              charge_counter_uah: 5470000
              capacity_percent: 0.3
              current_avg_ua: 25
              voltage_uv: 8460000.0
           }
           battery_aggregates {
              avg_power_mw: 6223.666666666667
              avg_power_from_charge_diff_mw: 2253.599999999949
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
