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

from python.generators.diff_tests.testing import Path, DataPath
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint, TraceInjector
from python.generators.diff_tests.testing import TestSuite


CREATE_PIVOT_TABLE_QUERY = """
  CREATE VIRTUAL TABLE pivot USING __intrinsic_pivot(
    '(SELECT * FROM slice)',
    'cat, name',
    'SUM(dur)'
  );
"""


class Pivot(TestSuite):
  def test_stuff(self):
    return DiffTestBlueprint(
        trace=DataPath('api34_startup_cold.perfetto-trace'),
        query=f"""
          {CREATE_PIVOT_TABLE_QUERY}
          SELECT cat, name, __id__, __parent_id__, __depth__, __has_children__, __child_count__, agg_0 FROM pivot
        """,
        out=Csv("""
"cat","name","__id__","__parent_id__","__depth__","__has_children__","__child_count__","agg_0"
"binder","[NULL]",1,0,0,1,4,5279798319.000000
"workqueue","[NULL]",2,0,0,1,33,160086968.000000
        """))
  
  def test_other_stuff(self):
    return DiffTestBlueprint(
        trace=DataPath('api34_startup_cold.perfetto-trace'),
        query=f"""
          {CREATE_PIVOT_TABLE_QUERY}
          SELECT cat, name, __id__, __parent_id__, __depth__, __has_children__, __child_count__, agg_0 FROM pivot
          WHERE __expanded_ids__ = '1'
          LIMIT 5
        """,
        out=Csv("""
"cat","name","__id__","__parent_id__","__depth__","__has_children__","__child_count__","agg_0"
"binder","binder transaction",5,1,1,0,0,2684362805.000000
"binder","binder reply",4,1,1,0,0,2595435514.000000
"binder","binder async rcv",3,1,1,0,0,0.000000
"binder","binder transaction async",6,1,1,0,0,0.000000
        """))

