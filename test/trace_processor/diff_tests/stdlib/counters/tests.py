#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

          WITH data(id, ts, value, track_id) AS (
            VALUES
            (0, 0, 10, 1),
            (1, 0, 10, 2),
            (2, 10, 10, 1),
            (3, 10, 20, 2),
            (4, 20, 30, 1)
          )
          SELECT * FROM counter_leading_intervals!(data);
        """,
        out=Csv("""
        "id","ts","dur","track_id","value","next_value","delta_value"
        0,0,10,1,10.000000,10.000000,"[NULL]"
        2,10,10,1,10.000000,30.000000,0.000000
        4,20,19980,1,30.000000,"[NULL]",20.000000
        1,0,10,2,10.000000,20.000000,"[NULL]"
        3,10,19990,2,20.000000,"[NULL]",10.000000
        """))

  def test_intervals_counter_leading_zero_deltas(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
        INCLUDE PERFETTO MODULE counters.intervals;

          WITH data(id, ts, value, track_id) AS (
            VALUES
            (1, 10, 10, 1),
            (2, 20, 11, 1),
            (3, 30, 11, 1),
            (4, 40, 11, 1),
            (5, 50, 11, 1),
            (6, 60, 12, 1)
          )
          SELECT * FROM counter_leading_intervals!(data);
        """,
        out=Csv("""
        "id","ts","dur","track_id","value","next_value","delta_value"
        1,10,10,1,10.000000,11.000000,"[NULL]"
        2,20,10,1,11.000000,11.000000,1.000000
        3,30,20,1,11.000000,11.000000,0.000000
        5,50,10,1,11.000000,12.000000,0.000000
        6,60,19940,1,12.000000,"[NULL]",1.000000
        """))
