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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from google.protobuf import text_format


class PreludeSlices(TestSuite):

  def test_slice_is_ancestor(self):
    return DiffTestBlueprint(
        trace=Path('nested_slices_trace.py'),
        query="""
        SELECT
          s1.name, s2.name, slice_is_ancestor(s1.id, s2.id) AS is_ancestor
        FROM slice s1
        JOIN slice s2
        WHERE s1.name < s2.name
      """,
        out=Csv("""
        "name","name","is_ancestor"
        "Slice 1","Slice 2",1
        "Slice 1","Slice 4",0
        "Slice 1","Slice 3",1
        "Slice 2","Slice 4",0
        "Slice 2","Slice 3",0
        "Slice 3","Slice 4",0
        """))
