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


class PerfettoInclude(TestSuite):

  def test_import(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        SELECT IMPORT('time.conversion');

        SELECT 1 AS x;
        """,
        out=Csv("""
        "x"
        1
        """))

  def test_include_perfetto_module(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        INCLUDE PERFETTO MODULE time.conversion;

        SELECT time_to_ns(1) AS x
        """,
        out=Csv("""
        "x"
        1
        """))

  def test_include_and_import(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        SELECT IMPORT('time.conversion');
        INCLUDE PERFETTO MODULE time.conversion;

        SELECT 1 AS x
        """,
        out=Csv("""
        "x"
        1
        """))
