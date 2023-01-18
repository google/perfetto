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

from python.generators.diff_tests.testing import Path, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Translation(DiffTestModule):

  def test_java_class_name_arg(self):
    return DiffTestBlueprint(
        trace=Path('java_class_name_arg.textproto'),
        query=Path('chrome_args_test.sql'),
        out=Path('java_class_name_arg.out'))

  def test_chrome_histogram(self):
    return DiffTestBlueprint(
        trace=Path('chrome_histogram.textproto'),
        query=Path('chrome_args_test.sql'),
        out=Path('chrome_histogram.out'))

  def test_chrome_user_event(self):
    return DiffTestBlueprint(
        trace=Path('chrome_user_event.textproto'),
        query=Path('chrome_args_test.sql'),
        out=Csv("""
"flat_key","key","int_value","string_value"
"is_root_in_scope","is_root_in_scope",1,"[NULL]"
"source","source","[NULL]","descriptor"
"source_id","source_id",12345,"[NULL]"
"chrome_user_event.action","chrome_user_event.action","[NULL]","action1"
"chrome_user_event.action_hash","chrome_user_event.action_hash",10,"[NULL]"
"chrome_user_event.action","chrome_user_event.action","[NULL]","action2"
"chrome_user_event.action_hash","chrome_user_event.action_hash",20,"[NULL]"
"chrome_user_event.action_hash","chrome_user_event.action_hash",30,"[NULL]"
"""))

  def test_chrome_performance_mark(self):
    return DiffTestBlueprint(
        trace=Path('chrome_performance_mark.textproto'),
        query=Path('chrome_args_test.sql'),
        out=Path('chrome_performance_mark.out'))

  def test_slice_name(self):
    return DiffTestBlueprint(
        trace=Path('slice_name.textproto'),
        query="""
SELECT name FROM slice ORDER BY name;
""",
        out=Csv("""
"name"
"mapped_name1"
"mapped_name2"
"raw_name3"
"slice_begin"
"""))

  def test_slice_name_2(self):
    return DiffTestBlueprint(
        trace=Path('slice_name_negative_timestamp.textproto'),
        query="""
SELECT name FROM slice ORDER BY name;
""",
        out=Csv("""
"name"
"mapped_name1"
"mapped_name2"
"raw_name3"
"slice_begin"
"""))

  def test_native_symbol_arg(self):
    return DiffTestBlueprint(
        trace=Path('native_symbol_arg.textproto'),
        query=Path('chrome_args_test.sql'),
        out=Path('native_symbol_arg.out'))

  def test_native_symbol_arg_2(self):
    return DiffTestBlueprint(
        trace=Path('native_symbol_arg_incomplete.textproto'),
        query=Path('chrome_args_test.sql'),
        out=Path('native_symbol_arg.out'))
