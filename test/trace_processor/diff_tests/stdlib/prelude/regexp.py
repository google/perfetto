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

from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from python.generators.diff_tests.testing import TextProto


class Regexp(TestSuite):

  def test_regexp_flags(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        SELECT
          'Malloc' REGEXP 'malloc' AS operator_sensitive,
          regexp('malloc', 'Malloc') AS function_sensitive,
          regexp('malloc', 'Malloc', 'i') AS insensitive,
          regexp('malloc', 'Malloc', 'c') AS explicit_sensitive,
          regexp('malloc', 'Malloc', 'ic') AS last_flag_sensitive,
          regexp('malloc', 'Malloc', 'ci') AS last_flag_insensitive;
        """,
        out=Csv("""
        "operator_sensitive","function_sensitive","insensitive","explicit_sensitive","last_flag_sensitive","last_flag_insensitive"
        0,0,1,0,0,1
        """))
