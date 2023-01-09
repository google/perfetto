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


class DiffTestModule_Startup(DiffTestModule):

  def test_android_startup(self):
    return DiffTestBlueprint(
        trace=Path('android_startup.py'),
        query=Path('android_startup'),
        out=Path('android_startup.out'))

  def test_android_startup_slow(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_slow.py'),
        query=Path('android_startup'),
        out=Path('android_startup_slow.out'))

  def test_android_startup_minsdk33(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_minsdk33.py'),
        query=Path('android_startup'),
        out=Path('android_startup_minsdk33.out'))

  def test_android_startup_breakdown(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_breakdown.py'),
        query=Path('android_startup'),
        out=Path('android_startup_breakdown.out'))

  def test_android_startup_breakdown_slow(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_breakdown_slow.py'),
        query=Path('android_startup'),
        out=Path('android_startup_breakdown_slow.out'))

  def test_android_startup_process_track(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_process_track.py'),
        query=Path('android_startup'),
        out=Path('android_startup_process_track.out'))

  def test_android_startup_attribution(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_attribution.py'),
        query=Path('android_startup'),
        out=Path('android_startup_attribution.out'))

  def test_android_startup_attribution_slow(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_attribution_slow.py'),
        query=Path('android_startup'),
        out=Path('android_startup_attribution_slow.out'))

  def test_android_startup_lock_contention(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_lock_contention.py'),
        query=Path('android_startup'),
        out=Path('android_startup_lock_contention.out'))

  def test_android_startup_lock_contention_slow(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_lock_contention_slow.py'),
        query=Path('android_startup'),
        out=Path('android_startup_lock_contention_slow.out'))

  def test_android_startup_installd_dex2oat(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_installd_dex2oat.py'),
        query=Path('android_startup'),
        out=Path('android_startup_installd_dex2oat.out'))

  def test_android_startup_installd_dex2oat_slow(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_installd_dex2oat_slow.py'),
        query=Path('android_startup'),
        out=Path('android_startup_installd_dex2oat_slow.out'))

  def test_android_startup_unlock(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_unlock.py'),
        query=Path('android_startup'),
        out=Path('android_startup_unlock.out'))

  def test_android_startup_broadcast(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_broadcast.py'),
        query=Path('android_startup'),
        out=Path('android_startup_broadcast.out'))

  def test_android_startup_broadcast_multiple(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_broadcast_multiple.py'),
        query=Path('android_startup'),
        out=Path('android_startup_broadcast_multiple.out'))

  def test_android_batt_counters(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_battery.py'),
        query=Path('android_batt'),
        out=Path('android_batt_counters.out'))

  def test_android_startup_cpu(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_cpu.py'),
        query=Path('android_cpu'),
        out=Path('android_startup_cpu.out'))

  def test_android_startup_powrails(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_powrails.py'),
        query=Path('android_powrails'),
        out=Path('android_startup_powrails.out'))
