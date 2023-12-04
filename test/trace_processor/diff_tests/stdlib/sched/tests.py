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


class StdlibSched(TestSuite):

  def test_runnable_thread_count(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query="""
      INCLUDE PERFETTO MODULE sched.thread_level_parallelism;
      SELECT * FROM sched_runnable_thread_count;
      """,
        out=Csv("""
      "ts","runnable_thread_count"
      1,1
      50,1
      100,2
      115,2
      120,2
      170,3
      250,2
      390,2
      """))

  def test_active_cpu_count(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query="""
      INCLUDE PERFETTO MODULE sched.thread_level_parallelism;
      SELECT * FROM sched_active_cpu_count;
      """,
        out=Csv("""
      "ts","active_cpu_count"
      1,1
      50,2
      100,2
      115,2
      120,2
      170,1
      250,2
      390,2
      """))
