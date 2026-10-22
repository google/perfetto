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

from python.generators.diff_tests.testing import Csv, DiffTestBlueprint, TestSuite, TextProto


class KeyValueLookup(TestSuite):

  def test_keyvalue_lookup_extract_quoted(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE android.keyvalue_lookup;
        SELECT _android_keyvalue_lookup_extract_key_value_arg(
          'key1="value 1" key2="value 2" key3="value3"',
          'key2'
        ) AS val;
        """,
        out=Csv("""
        "val"
        "value 2"
        """))

  def test_keyvalue_lookup_extract_unquoted(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE android.keyvalue_lookup;
        SELECT _android_keyvalue_lookup_extract_key_value_arg(
          'key1=value1 key2=value2 key3=value3',
          'key2'
        ) AS val1,
        _android_keyvalue_lookup_extract_key_value_arg(
          '{key3=value3}',
          'key3'
        ) AS val2;
        """,
        out=Csv("""
        "val1","val2"
        "value2","value3"
        """))

  def test_keyvalue_lookup_extract_missing_or_null(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE android.keyvalue_lookup;
        SELECT _android_keyvalue_lookup_extract_key_value_arg(
          'key1=value1 key2=value2',
          'key3'
        ) AS val1,
        _android_keyvalue_lookup_extract_key_value_arg(
          'key1 key2',
          'key1'
        ) AS val2;
        """,
        out=Csv("""
        "val1","val2"
        "[NULL]","[NULL]"
        """))
