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
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from google.protobuf import text_format


class Timestamps(TestSuite):

  def test_to_time(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE time.conversion;

        WITH data(unit, time) AS (
          VALUES
            ('ns', time_to_ns(cast_int!(1e14))),
            ('us', time_to_us(cast_int!(1e14))),
            ('ms', time_to_ms(cast_int!(1e14))),
            ('s', time_to_s(cast_int!(1e14))),
            ('min', time_to_min(cast_int!(1e14))),
            ('h', time_to_hours(cast_int!(1e14))),
            ('days', time_to_days(cast_int!(1e14)))
        )
        SELECT * FROM data
      """,
        out=Csv("""
        "unit","time"
        "ns",100000000000000
        "us",100000000000
        "ms",100000000
        "s",100000
        "min",1666
        "h",27
        "days",1
      """))

  def test_from_time(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE time.conversion;

        WITH data(unit, time) AS (
          VALUES
            ('ns', time_from_ns(1)),
            ('us', time_from_us(1)),
            ('ms', time_from_ms(1)),
            ('s', time_from_s(1)),
            ('min', time_from_min(1)),
            ('h', time_from_hours(1)),
            ('days', time_from_days(1))
        )
        SELECT * FROM data
      """,
        out=Csv("""
        "unit","time"
        "ns",1
        "us",1000
        "ms",1000000
        "s",1000000000
        "min",60000000000
        "h",3600000000000
        "days",86400000000000
      """))