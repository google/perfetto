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


class SmokeJson(TestSuite):
  # Contains smoke tests which test the most fundamentally important features
  # trace processor  Note: new tests here should only be added by the Perfetto
  # JSON trace parsing
  def test_sfgate_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('sfgate.json'),
        query="""
        SELECT
          ts,
          cpu,
          dur,
          end_state,
          priority,
          tid
        FROM sched
        JOIN thread USING(utid)
        ORDER BY ts
        LIMIT 10;
        """,
        out=Csv("""
        "ts","cpu","dur","end_state","priority","tid"
        """))

  def test_sfgate_smoke_slices(self):
    return DiffTestBlueprint(
        trace=DataPath('sfgate.json'),
        query="""
        SELECT depth, count(*) AS count
        FROM slice
        JOIN track ON slice.track_id = track.id
        GROUP BY depth
        ORDER BY depth;
        """,
        out=Csv("""
        "depth","count"
        0,16888
        1,19447
        2,5816
        3,829
        4,191
        5,94
        6,57
        7,19
        8,14
        9,2
        """))
