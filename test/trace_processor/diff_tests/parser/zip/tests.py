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

from python.generators.diff_tests.testing import Csv, Path, DataPath
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Zip(TestSuite):

  def test_perf_proto_sym(self):
    return DiffTestBlueprint(
        trace=DataPath('zip/perf_track_sym.zip'),
        query=Path('stacks_test.sql'),
        out=Csv('''
        "name"
        "main,A"
        "main,A,B"
        "main,A,B,C"
        "main,A,B,C,D"
        "main,A,B,C,D,E"
        "main,A,B,C,E"
        "main,A,B,D"
        "main,A,B,D,E"
        "main,A,B,E"
        "main,A,C"
        "main,A,C,D"
        "main,A,C,D,E"
        "main,A,C,E"
        "main,A,D"
        "main,A,D,E"
        "main,A,E"
        "main,B"
        "main,B,C"
        "main,B,C,D"
        "main,B,C,D,E"
        "main,B,C,E"
        "main,B,D"
        "main,B,D,E"
        "main,B,E"
        "main,C"
        "main,C,D"
        "main,C,D,E"
        "main,C,E"
        "main,D"
        "main,D,E"
        "main,E"
        '''))
