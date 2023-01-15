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
        out=Path('chrome_user_event.out'))

  def test_chrome_performance_mark(self):
    return DiffTestBlueprint(
        trace=Path('chrome_performance_mark.textproto'),
        query=Path('chrome_args_test.sql'),
        out=Path('chrome_performance_mark.out'))

  def test_slice_name(self):
    return DiffTestBlueprint(
        trace=Path('slice_name.textproto'),
        query=Path('slice_name_test.sql'),
        out=Path('slice_name.out'))

  def test_slice_name_2(self):
    return DiffTestBlueprint(
        trace=Path('slice_name_negative_timestamp.textproto'),
        query=Path('slice_name_test.sql'),
        out=Path('slice_name.out'))

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
