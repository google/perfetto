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
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from google.protobuf import text_format


class Timestamps(TestSuite):

  def test_ns(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE common.timestamps;
        SELECT ns(4) as result;
      """,
        out=Csv("""
        "result"
        4
      """))

  def test_us(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE common.timestamps;
        SELECT us(4) as result;
      """,
        out=Csv("""
        "result"
        4000
      """))

  def test_ms(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE common.timestamps;
        SELECT ms(4) as result;
      """,
        out=Csv("""
        "result"
        4000000
      """))

  def test_seconds(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE common.timestamps;
        SELECT seconds(4) as result;
      """,
        out=Csv("""
        "result"
        4000000000
      """))

  def test_minutes(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE common.timestamps;
        SELECT minutes(1) as result;
      """,
        out=Csv("""
        "result"
        60000000000
      """))

  def test_hours(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE common.timestamps;
        SELECT hours(1) as result;
      """,
        out=Csv("""
        "result"
        3600000000000
      """))

  def test_days(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE common.timestamps;
        SELECT days(1) as result;
      """,
        out=Csv("""
        "result"
        86400000000000
      """))
