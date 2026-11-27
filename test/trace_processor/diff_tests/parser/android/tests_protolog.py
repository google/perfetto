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
from python.generators.diff_tests.testing import TextProto


class ProtoLog(TestSuite):

  def test_has_expected_protolog_rows(self):
    return DiffTestBlueprint(
        trace=Path("protolog.textproto"),
        query="SELECT id, ts, level, tag, message, location, stacktrace FROM protolog;",
        out=Csv("""
        "id","ts","level","tag","message","location","stacktrace"
        0,857384100,"DEBUG","MyFirstGroup","Test message with a string (MyTestString), an int (888), a double 8.88, and a boolean true.","com/test/TestClass.java:123","A STACK TRACE"
        1,857384110,"WARN","MySecondGroup","Test message with different int formats: 888, 0o1570, 0x378, 888.000000, 8.880000e+02.","com/test/TestClass.java:567","[NULL]"
        2,857384130,"ERROR","MyThirdGroup","Message re-using interned string 'MyOtherTestString' == 'MyOtherTestString', but 'SomeOtherTestString' != 'MyOtherTestString'","com/test/TestClass.java:527","[NULL]"
        3,857384140,"VERBOSE","MyNonProcessedGroup","My non-processed proto message with a string (MyTestString), an int (888), a double 8.88, and a boolean true.","[NULL]","[NULL]"
        4,857384150,"DEBUG","MyFirstGroup","Test message with percent sign: % and a string MyTestString","com/test/TestClass.java:888","[NULL]"
        """),
    )

  def test_handles_packet_loss(self):
    return DiffTestBlueprint(
        trace=Path("protolog_packet_loss.textproto"),
        query="SELECT id, ts, level, tag, message, location, stacktrace FROM protolog;",
        out=Csv("""
        "id","ts","level","tag","message","location","stacktrace"
        0,857384130,"DEBUG","MyFirstGroup","Test message with two strings: MyTestString and MyTestString","com/test/TestClass.java:123","[NULL]"
        1,857384130,"DEBUG","MyFirstGroup","Test message with two strings: MyNextTestString and MyNextTestString","com/test/TestClass.java:123","[NULL]"
        """),
    )

  def test_duplicate_viewer_config_bytes(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          [com.android.internal.FrameworksNativeWinscopeTracePacket.protolog_viewer_config] {
            groups { id: 1 name: "WM" tag: "WM" }
            messages {
              message_id: 100
              message: "Val: %d"
              level: PROTOLOG_LEVEL_INFO
              group_id: 1
              location: "Foo.java:10"
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          [com.android.internal.FrameworksNativeWinscopeTracePacket.protolog_viewer_config] {
            groups { id: 1 name: "WM" tag: "WM" }
            messages {
              message_id: 100
              message: "Val: %d"
              level: PROTOLOG_LEVEL_INFO
              group_id: 1
              location: "Foo.java:10"
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          [com.android.internal.FrameworksNativeWinscopeTracePacket.protolog_viewer_config] {
            groups { id: 1 name: "WM" tag: "WM" }
            messages {
              message_id: 100
              message: "Val: %d"
              level: PROTOLOG_LEVEL_INFO
              group_id: 1
              location: "Foo.java:10"
            }
            messages {
              message_id: 200
              message: "New"
              level: PROTOLOG_LEVEL_WARN
              group_id: 1
            }
          }
        }
        """),
        query="""
        SELECT
          l.name,
          l.severity,
          l.byte_offset > 0 AS has_byte_offset,
          a.key,
          a.int_value,
          s.value AS stat_count
        FROM _trace_import_logs AS l
        JOIN args AS a USING (arg_set_id)
        JOIN stats AS s USING (name)
        WHERE l.name = 'winscope_protolog_viewer_config_duplicates'
        ORDER BY l.id;
        """,
        out=Csv("""
        "name","severity","has_byte_offset","key","int_value","stat_count"
        "winscope_protolog_viewer_config_duplicates","info",1,"duplicate_bytes",45,2
        "winscope_protolog_viewer_config_duplicates","info",1,"duplicate_bytes",45,2
        """),
    )
