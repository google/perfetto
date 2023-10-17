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
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class SpanJoinSmoke(TestSuite):

  def test_span_join_unordered_cols_synth_1(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query=Path('span_join_unordered_cols_test.sql'),
        out=Csv("""
        "ts","dur","part","b1","b2","b3","a1","a2","a3"
        10,90,0,"A",10,100,"B",2,101
        100,1,0,"B",90,200,"C",3,102
        5,5,1,"A",10,100,"A",1,100
        10,40,1,"A",10,100,"B",2,101
        50,40,1,"B",90,200,"B",2,101
        90,10,1,"C",1,300,"B",2,101
        100,1,1,"C",1,300,"C",3,102
        """))

  def test_span_join_unordered_cols_synth_1_2(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query=Path('span_join_unordered_cols_reverse_test.sql'),
        out=Csv("""
        "ts","dur","part","b1","b2","b3","a1","a2","a3"
        10,90,0,"A",10,100,"B",2,101
        100,1,0,"B",90,200,"C",3,102
        5,5,1,"A",10,100,"A",1,100
        10,40,1,"A",10,100,"B",2,101
        50,40,1,"B",90,200,"B",2,101
        90,10,1,"C",1,300,"B",2,101
        100,1,1,"C",1,300,"C",3,102
        """))

  def test_span_join_zero_negative_dur(self):
    return DiffTestBlueprint(
        trace=DataPath('android_sched_and_ps.pb'),
        query=Path('span_join_zero_negative_dur_test.sql'),
        out=Csv("""
        "ts","dur","part"
        1,0,0
        1,2,0
        5,-1,0
        5,-1,0
        1,1,1
        2,0,1
        """))
