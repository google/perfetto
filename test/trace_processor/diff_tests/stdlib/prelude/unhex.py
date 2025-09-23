#!/usr/bin/env python3
#
# Copyright (C) 2025 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto, RawText
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class UnHex(TestSuite):

  def test_unhex(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        SELECT
          unhex('0xF') AS with_prefix,
          unhex('F') AS without_prefix,
          unhex('\t  0Xf    \n\r\f\v') AS with_space,
          unhex('0') AS zero,
          unhex(NULL) AS null_param,
          unhex('0x58646cfa') AS big
        """,
        out=Csv("""
        "with_prefix","without_prefix","with_space","zero","null_param","big"
        15,15,15,0,"[NULL]",1482976506
        """))
