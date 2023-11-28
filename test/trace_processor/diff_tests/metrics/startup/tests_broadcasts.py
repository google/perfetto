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


class StartupBroadcasts(TestSuite):
  # Test that broadcasts are correctly counted.
  def test_android_startup_broadcast(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_broadcast.py'),
        query=Metric('android_startup'),
        out=Path('android_startup_broadcast.out'))

  def test_android_startup_broadcast_multiple(self):
    return DiffTestBlueprint(
        trace=Path('android_startup_broadcast_multiple.py'),
        query=Metric('android_startup'),
        out=Path('android_startup_broadcast_multiple.out'))
