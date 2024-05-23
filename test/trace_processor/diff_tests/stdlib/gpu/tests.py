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

from python.generators.diff_tests.testing import Path, DataPath, Metric, Systrace
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from python.generators.diff_tests.testing import PrintProfileProto


class Gpu(TestSuite):

  def test_gpu_frequency(self):
    return DiffTestBlueprint(
        trace=Path('../../metrics/graphics/gpu_frequency_metric.textproto'),
        query="""
        INCLUDE PERFETTO MODULE gpu.frequency;
        SELECT *
        FROM gpu_frequency;
      """,
        out=Csv("""
        "ts","dur","gpu_id","gpu_freq"
        200001000000,2000000,0,585000
        200003000000,1000000,0,0
        200004000000,2000000,0,603000
        200002000000,3000000,1,400000
        200005000000,1000000,1,758000
      """))
