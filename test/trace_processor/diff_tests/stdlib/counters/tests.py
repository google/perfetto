#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class StdlibCounterIntervals(TestSuite):

  def test_intervals_counter_leading(self):
    return DiffTestBlueprint(
      trace=DataPath('counters.json'),
        query="""
        INCLUDE PERFETTO MODULE counters.intervals;

        WITH
          foo AS (
            SELECT 0 AS id, 0 AS ts, 10 AS value, 1 AS track_id
            UNION ALL
            SELECT 1 AS id, 0 AS ts, 10 AS value, 2 AS track_id
            UNION ALL
            SELECT 2 AS id, 10 AS ts, 10 AS value, 1 AS track_id
            UNION ALL
            SELECT 3 AS id, 10 AS ts, 20 AS value, 2 AS track_id
            UNION ALL
            SELECT 4 AS id, 20 AS ts, 30 AS value, 1 AS track_id
          )
        SELECT * FROM counter_leading_intervals !(foo);
        """,
        out=Csv("""
        "id","ts","track_id","dur","value"
        0,0,1,20,10
        4,20,1,19980,30
        1,0,2,10,10
        3,10,2,19990,20
        """))
