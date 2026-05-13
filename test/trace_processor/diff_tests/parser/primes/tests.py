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

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class PrimesTraceParser(TestSuite):

  def test_primes_trace_slice_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('primes_trace_for_test.primestrace'),
        query="""
          SELECT id, ts, dur, track_id, name, slice_id
          FROM slice
          ORDER BY dur DESC
          LIMIT 10
        """,
        out=Csv('''
          "id","ts","dur","track_id","name","slice_id"
          0,1770756106884713625,207370958,0,"Trace_1",0
          5,1770756106899977333,178825250,2,"Entity_3",5
          414,1770756106965413042,97789916,5,"Entity_170",414
          183,1770756106935184042,62630250,79,"Entity_94",183
          8,1770756106903541417,53630958,4,"Entity_6",8
          477,1770756106974937250,53480042,151,"Entity_189",477
          9,1770756106903668833,51949459,5,"Entity_7",9
          481,1770756106975265833,51887000,152,"Entity_190",481
          11,1770756106903803083,51811834,6,"Entity_9",11
          482,1770756106975287625,36242792,154,"Entity_191",482
        '''))

  def test_primes_trace_track_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('primes_trace_for_test.primestrace'),
        query="""
        SELECT id, name, track_group_id
        FROM track
        WHERE name IS NOT null
        LIMIT 10;
      """,
        out=Csv('''
        "id","name","track_group_id"
        0,"Executor_1",0
        14,"Executor_2",1
        18,"Executor_3",2
        19,"Executor_4",3
        21,"Executor_5",4
        28,"Executor_6",5
        31,"Executor_7",6
        55,"Executor_8",7
        65,"Executor_9",8
        73,"Executor_10",9
      '''))
