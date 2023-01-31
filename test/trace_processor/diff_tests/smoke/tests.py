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


class Smoke(TestSuite):
  # Contains smoke tests which test the most fundamentally important features
  # trace processor  Note: new tests here should only be added by the Perfetto
  # Compresesed traces
  def test_compressed_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('compressed.pb'),
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
        170601497673450,2,53646,"DK",120,6790
        170601497691210,7,22917,"R",120,0
        170601497714127,7,29167,"D",120,6732
        170601497727096,2,55156,"S",120,62
        170601497743294,7,862656,"R",120,0
        170601497766106,3,13594,"S",120,8
        170601497779700,3,31094,"D",120,6790
        170601497782252,2,875313,"R",120,0
        170601497810794,3,824635,"R",120,0
        170601498605950,7,158333,"D",120,6732
        """))
