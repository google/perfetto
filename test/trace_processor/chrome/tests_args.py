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
from python.generators.diff_tests.testing import TestSuite


class ChromeArgs(TestSuite):

  def test_unsymbolized_args(self):
    return DiffTestBlueprint(
        trace=Path('unsymbolized_args.textproto'),
        query=Metric('chrome_unsymbolized_args'),
        out=TextProto(r"""
[perfetto.protos.chrome_unsymbolized_args]: {
  args {
     module: "/liblib.so"
     build_id: "6275696c642d6964"
     address: 123
     google_lookup_id: "6275696c642d6964"
   }
   args {
     module: "/libmonochrome_64.so"
     build_id: "7f0715c286f8b16c10e4ad349cda3b9b56c7a773"
     address: 234
     google_lookup_id: "c215077ff8866cb110e4ad349cda3b9b0"
   }
}"""))

  def test_async_trace_1_count_slices(self):
    return DiffTestBlueprint(
        trace=Path('../../data/async-trace-1.json'),
        query="""
        SELECT COUNT(1) FROM slice;
        """,
        out=Csv("""
        "COUNT(1)"
        16
        """))

  def test_async_trace_2_count_slices(self):
    return DiffTestBlueprint(
        trace=Path('../../data/async-trace-2.json'),
        query="""
        SELECT COUNT(1) FROM slice;
        """,
        out=Csv("""
        "COUNT(1)"
        35
        """))

  def test_chrome_args_class_names(self):
    return DiffTestBlueprint(
        trace=Path('chrome_args_class_names.textproto'),
        query=Metric('chrome_args_class_names'),
        out=TextProto(r"""

[perfetto.protos.chrome_args_class_names] {
  class_names_per_version {
    class_name: "abc"
    class_name: "def"
    class_name: "ghi"
    class_name: "jkl"
  }
}
"""))
