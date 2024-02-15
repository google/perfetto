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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ProtoLog(TestSuite):

  def test_has_expected_protolog_rows(self):
    return DiffTestBlueprint(
        trace=Path('protolog.textproto'),
        query="SELECT id, ts, level, tag, message, stacktrace FROM protolog;",
        out=Csv("""
        "id","ts","level","tag","message","stacktrace"
        0,857384100,"DEBUG","MyFirstGroup","Test message with a string (MyTestString), an int (1776), a double 8.88, and a boolean true.","A STACK TRACE"
        1,857384110,"WARN","MySecondGroup","Test message with different int formats: 1776, 0o3360, 0x6f0, 888.000000, 8.880000e+02.","[NULL]"
        2,857384130,"ERROR","MyThirdGroup","Message re-using interned string 'MyOtherTestString' == 'MyOtherTestString', but 'SomeOtherTestString' != 'MyOtherTestString'","[NULL]"
        """))
