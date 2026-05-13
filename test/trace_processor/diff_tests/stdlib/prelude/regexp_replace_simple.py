#!/usr/bin/env python3
#
# Copyright (C) 2026 The Android Open Source Project
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


class RegexpReplaceSimple(TestSuite):

  def test_regexp_replace_simple(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        SELECT
          REGEXP_REPLACE_SIMPLE('abcde', 'c', 'C') AS c,
          REGEXP_REPLACE_SIMPLE('abcde', 'i', 'I') AS i,
          REGEXP_REPLACE_SIMPLE('abcde', '', '|') AS interpolate
        """,
        out=Csv("""
        "c","i","interpolate"
        "abCde","abcde","|a|b|c|d|e|"
        """))
