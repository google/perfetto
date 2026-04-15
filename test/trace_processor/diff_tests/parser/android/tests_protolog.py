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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ProtoLog(TestSuite):

  def test_has_expected_protolog_rows(self):
    return DiffTestBlueprint(
        trace=Path('protolog.textproto'),
        query="SELECT id, ts, level, tag, message, location, stacktrace FROM protolog;",
        out=Csv("""
        "id","ts","level","tag","message","location","stacktrace"
        0,857384100,"DEBUG","MyFirstGroup","Test message with a string (MyTestString), an int (888), a double 8.88, and a boolean true.","com/test/TestClass.java:123","A STACK TRACE"
        1,857384110,"WARN","MySecondGroup","Test message with different int formats: 888, 0o1570, 0x378, 888.000000, 8.880000e+02.","com/test/TestClass.java:567","[NULL]"
        2,857384130,"ERROR","MyThirdGroup","Message re-using interned string 'MyOtherTestString' == 'MyOtherTestString', but 'SomeOtherTestString' != 'MyOtherTestString'","com/test/TestClass.java:527","[NULL]"
        3,857384140,"VERBOSE","MyNonProcessedGroup","My non-processed proto message with a string (MyTestString), an int (888), a double 8.88, and a boolean true.","[NULL]","[NULL]"
        """))

  def test_handles_packet_loss(self):
    return DiffTestBlueprint(
        trace=Path('protolog_packet_loss.textproto'),
        query="SELECT id, ts, level, tag, message, location, stacktrace FROM protolog;",
        out=Csv("""
        "id","ts","level","tag","message","location","stacktrace"
        0,857384130,"DEBUG","MyFirstGroup","Test message with two strings: MyTestString and MyTestString","com/test/TestClass.java:123","[NULL]"
        1,857384130,"DEBUG","MyFirstGroup","Test message with two strings: MyTestString and MyOtherTestString","com/test/TestClass.java:123","[NULL]"
        2,857384130,"DEBUG","MyFirstGroup","Test message with two strings: [LOST_STRING] and [LOST_STRING]","com/test/TestClass.java:123","[NULL]"
        3,857384130,"DEBUG","MyFirstGroup","Test message with two strings: MyNextTestString and MyNextTestString","com/test/TestClass.java:123","[NULL]"
        """))

  def test_missing_config(self):
    return DiffTestBlueprint(
        trace=Path('protolog_missing_config.textproto'),
        query="SELECT id, ts, level, tag, message, stacktrace, location FROM protolog;",
        out=Csv("""
        "id","ts","level","tag","message","stacktrace","location"
        0,857384130,"UNKNOWN","UNKNOWN","[MISSING_VIEWER_CONFIG] (ID: 6924537961316301726)","[NULL]","[NULL]"
        """))

  def test_multisequence_loss(self):
    return DiffTestBlueprint(
        trace=Path('protolog_multisequence_loss.textproto'),
        query="SELECT id, ts, level, tag, message, stacktrace, location FROM protolog;",
        out=Csv("""
        "id","ts","level","tag","message","stacktrace","location"
        0,857384130,"DEBUG","MyFirstGroup","Test message with two strings: Seq1String and Seq1String","[NULL]","com/test/TestClass.java:123"
        1,857384131,"DEBUG","MyFirstGroup","Test message with two strings: [LOST_STRING] and [LOST_STRING]","[NULL]","com/test/TestClass.java:123"
        """))
