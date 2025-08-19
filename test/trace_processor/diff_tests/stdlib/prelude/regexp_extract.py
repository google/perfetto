#!/usr/bin/env python3
#
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


class RegexpExtract(TestSuite):

  def test_regexp_extract(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        SELECT
          REGEXP_EXTRACT('abcde', 'b(c)d') AS c,
          REGEXP_EXTRACT('abcde', 'b(c)d') AS c_again,
          REGEXP_EXTRACT('abcde', 'a(b)cde') AS b,
          REGEXP_EXTRACT('abcde', 'a(b)cde') AS b_again,
          REGEXP_EXTRACT('abcde', 'fgh') AS no_match,
          REGEXP_EXTRACT('abc', 'a(b)?c') AS optional_match,
          REGEXP_EXTRACT('ac', 'a(b)?c') AS optional_no_match
        """,
        out=Csv("""
        "c","c_again","b","b_again","no_match","optional_match","optional_no_match"
        "c","c","b","b","[NULL]","b","ac"
        """))
