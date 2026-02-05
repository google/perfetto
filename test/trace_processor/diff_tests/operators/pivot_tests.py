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

CREATE_TEST_TABLE = """
  CREATE PERFETTO TABLE sales AS
  WITH data(category, item, value) AS (
    VALUES
      ('fruit', 'apple', 10),
      ('fruit', 'apple', 20),
      ('fruit', 'banana', 15),
      ('vegetable', 'carrot', 5),
      ('vegetable', 'carrot', 8),
      ('vegetable', 'potato', 12)
  )
  SELECT * FROM data;
"""

CREATE_PIVOT_TABLE_QUERY = """
  CREATE VIRTUAL TABLE pivot USING __intrinsic_rollup_tree(
    '(SELECT * FROM sales)',
    'category, item',
    'SUM(value)'
  );
"""


class Pivot(TestSuite):

  def test_pivot_basic(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_PIVOT_TABLE_QUERY}
          SELECT category, item, __depth, __child_count, __agg_0 FROM pivot
        """,
        out=Csv("""
"category","item","__depth","__child_count","__agg_0"
"[NULL]","[NULL]",0,2,70
"fruit","[NULL]",1,2,45
"fruit","apple",2,0,30
"fruit","banana",2,0,15
"vegetable","[NULL]",1,2,25
"vegetable","carrot",2,0,13
"vegetable","potato",2,0,12
        """))

  def test_pivot_expanded(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_PIVOT_TABLE_QUERY}
          SELECT category, item, __depth, __child_count, __agg_0 FROM pivot
          WHERE __expanded_ids = '1'
        """,
        out=Csv("""
"category","item","__depth","__child_count","__agg_0"
"[NULL]","[NULL]",0,2,70
"fruit","[NULL]",1,2,45
"fruit","apple",2,0,30
"fruit","banana",2,0,15
"vegetable","[NULL]",1,2,25
        """))

  def test_pivot_collapsed(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_PIVOT_TABLE_QUERY}
          SELECT category, item, __depth, __child_count, __agg_0 FROM pivot
          WHERE __collapsed_ids = '1'
        """,
        out=Csv("""
"category","item","__depth","__child_count","__agg_0"
"[NULL]","[NULL]",0,2,70
"fruit","[NULL]",1,2,45
"vegetable","[NULL]",1,2,25
"vegetable","carrot",2,0,13
"vegetable","potato",2,0,12
        """))

  def test_pivot_sort_asc(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_PIVOT_TABLE_QUERY}
          SELECT category, item, __depth, __agg_0 FROM pivot
          WHERE __sort = '__agg_0 ASC'
        """,
        out=Csv("""
"category","item","__depth","__agg_0"
"[NULL]","[NULL]",0,70
"vegetable","[NULL]",1,25
"vegetable","potato",2,12
"vegetable","carrot",2,13
"fruit","[NULL]",1,45
"fruit","banana",2,15
"fruit","apple",2,30
        """))

  def test_pivot_sort_by_name(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_PIVOT_TABLE_QUERY}
          SELECT category, item, __depth, __agg_0 FROM pivot
          WHERE __sort = 'name ASC'
        """,
        out=Csv("""
"category","item","__depth","__agg_0"
"[NULL]","[NULL]",0,70
"fruit","[NULL]",1,45
"fruit","apple",2,30
"fruit","banana",2,15
"vegetable","[NULL]",1,25
"vegetable","carrot",2,13
"vegetable","potato",2,12
        """))

  def test_pivot_limit(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_PIVOT_TABLE_QUERY}
          SELECT category, item, __depth, __agg_0 FROM pivot
          WHERE __expanded_ids = '1,2' AND __limit = 3
        """,
        out=Csv("""
"category","item","__depth","__agg_0"
"[NULL]","[NULL]",0,70
"fruit","[NULL]",1,45
"fruit","apple",2,30
        """))

  def test_pivot_offset_and_limit(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_PIVOT_TABLE_QUERY}
          SELECT category, item, __depth, __agg_0 FROM pivot
          WHERE __expanded_ids = '1,2' AND __offset = 2 AND __limit = 2
        """,
        out=Csv("""
"category","item","__depth","__agg_0"
"fruit","apple",2,30
"fruit","banana",2,15
        """))

  def test_pivot_multiple_aggregates(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          CREATE VIRTUAL TABLE pivot_multi USING __intrinsic_rollup_tree(
            '(SELECT * FROM sales)',
            'category, item',
            'SUM(value), COUNT(*), AVG(value)'
          );
          SELECT category, item, __depth, __agg_0, __agg_1, __agg_2 FROM pivot_multi
        """,
        out=Csv("""
"category","item","__depth","__agg_0","__agg_1","__agg_2"
"[NULL]","[NULL]",0,70,6,11.666667
"fruit","[NULL]",1,45,3,15.000000
"fruit","apple",2,30,2,15.000000
"fruit","banana",2,15,1,15.000000
"vegetable","[NULL]",1,25,3,8.333333
"vegetable","carrot",2,13,2,6.500000
"vegetable","potato",2,12,1,12.000000
        """))

  def test_pivot_expand_multiple(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_PIVOT_TABLE_QUERY}
          SELECT category, item, __depth, __agg_0 FROM pivot
          WHERE __expanded_ids = '1,2'
        """,
        out=Csv("""
"category","item","__depth","__agg_0"
"[NULL]","[NULL]",0,70
"fruit","[NULL]",1,45
"fruit","apple",2,30
"fruit","banana",2,15
"vegetable","[NULL]",1,25
"vegetable","carrot",2,13
"vegetable","potato",2,12
        """))

  def test_pivot_table_name_input(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          CREATE VIRTUAL TABLE pivot_direct USING __intrinsic_rollup_tree(
            'sales',
            'category, item',
            'SUM(value)'
          );
          SELECT category, item, __depth, __agg_0 FROM pivot_direct
        """,
        out=Csv("""
"category","item","__depth","__agg_0"
"[NULL]","[NULL]",0,70
"fruit","[NULL]",1,45
"fruit","apple",2,30
"fruit","banana",2,15
"vegetable","[NULL]",1,25
"vegetable","carrot",2,13
"vegetable","potato",2,12
        """))

  def test_pivot_sort_expanded_asc(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_PIVOT_TABLE_QUERY}
          SELECT category, item, __depth, __agg_0 FROM pivot
          WHERE __expanded_ids = '1,2' AND __sort = '__agg_0 ASC'
        """,
        out=Csv("""
"category","item","__depth","__agg_0"
"[NULL]","[NULL]",0,70
"vegetable","[NULL]",1,25
"vegetable","potato",2,12
"vegetable","carrot",2,13
"fruit","[NULL]",1,45
"fruit","banana",2,15
"fruit","apple",2,30
        """))

  def test_pivot_sort_expanded_desc(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_PIVOT_TABLE_QUERY}
          SELECT category, item, __depth, __agg_0 FROM pivot
          WHERE __expanded_ids = '1,2' AND __sort = '__agg_0 DESC'
        """,
        out=Csv("""
"category","item","__depth","__agg_0"
"[NULL]","[NULL]",0,70
"fruit","[NULL]",1,45
"fruit","apple",2,30
"fruit","banana",2,15
"vegetable","[NULL]",1,25
"vegetable","carrot",2,13
"vegetable","potato",2,12
        """))

  def test_pivot_offset_only(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_PIVOT_TABLE_QUERY}
          SELECT category, item, __depth, __agg_0 FROM pivot
          WHERE __expanded_ids = '1,2' AND __offset = 4
        """,
        out=Csv("""
"category","item","__depth","__agg_0"
"vegetable","[NULL]",1,25
"vegetable","carrot",2,13
"vegetable","potato",2,12
        """))

  def test_pivot_limit_with_sort(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_PIVOT_TABLE_QUERY}
          SELECT category, item, __depth, __agg_0 FROM pivot
          WHERE __expanded_ids = '1,2' AND __sort = '__agg_0 ASC' AND __limit = 3
        """,
        out=Csv("""
"category","item","__depth","__agg_0"
"[NULL]","[NULL]",0,70
"vegetable","[NULL]",1,25
"vegetable","potato",2,12
        """))

  def test_pivot_offset_limit_with_sort(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_PIVOT_TABLE_QUERY}
          SELECT category, item, __depth, __agg_0 FROM pivot
          WHERE __expanded_ids = '1,2' AND __sort = '__agg_0 ASC' AND __offset = 1 AND __limit = 3
        """,
        out=Csv("""
"category","item","__depth","__agg_0"
"vegetable","[NULL]",1,25
"vegetable","potato",2,12
"vegetable","carrot",2,13
        """))

  def test_pivot_offset_past_data(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=f"""
          {CREATE_TEST_TABLE}
          {CREATE_PIVOT_TABLE_QUERY}
          SELECT category, item, __depth, __agg_0 FROM pivot
          WHERE __offset = 100
        """,
        out=Csv("""
"category","item","__depth","__agg_0"
        """))

  def test_pivot_integer_groupby(self):
    """Test grouping by integer columns preserves integer type."""
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE PERFETTO TABLE int_data AS
          WITH data(region_id, store_id, sales) AS (
            VALUES
              (1, 100, 50),
              (1, 100, 30),
              (1, 200, 40),
              (2, 300, 60),
              (2, 300, 20)
          )
          SELECT * FROM data;

          CREATE VIRTUAL TABLE int_pivot USING __intrinsic_rollup_tree(
            '(SELECT * FROM int_data)',
            'region_id, store_id',
            'SUM(sales)'
          );

          SELECT region_id, store_id, __depth, __agg_0 FROM int_pivot
        """,
        out=Csv("""
"region_id","store_id","__depth","__agg_0"
"[NULL]","[NULL]",0,200
1,"[NULL]",1,120
1,100,2,80
1,200,2,40
2,"[NULL]",1,80
2,300,2,80
        """))

  def test_pivot_null_groupby_values(self):
    """Test that NULL values in groupby columns are handled correctly."""
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE PERFETTO TABLE null_data AS
          WITH data(category, subcategory, amount) AS (
            VALUES
              ('A', 'x', 10),
              ('A', NULL, 20),
              (NULL, 'y', 30),
              (NULL, NULL, 40)
          )
          SELECT * FROM data;

          CREATE VIRTUAL TABLE null_pivot USING __intrinsic_rollup_tree(
            '(SELECT * FROM null_data)',
            'category, subcategory',
            'SUM(amount)'
          );

          SELECT category, subcategory, __depth, __agg_0 FROM null_pivot
        """,
        out=Csv("""
"category","subcategory","__depth","__agg_0"
"[NULL]","[NULL]",0,100
"[NULL]","[NULL]",1,70
"[NULL]","[NULL]",2,40
"[NULL]","y",2,30
"A","[NULL]",1,30
"A","[NULL]",2,20
"A","x",2,10
        """))

  def test_pivot_real_groupby(self):
    """Test grouping by REAL (floating point) columns."""
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE PERFETTO TABLE real_data AS
          WITH data(price_tier, quantity) AS (
            VALUES
              (1.5, 10),
              (1.5, 20),
              (2.5, 30),
              (2.5, 15)
          )
          SELECT * FROM data;

          CREATE VIRTUAL TABLE real_pivot USING __intrinsic_rollup_tree(
            '(SELECT * FROM real_data)',
            'price_tier',
            'SUM(quantity)'
          );

          SELECT price_tier, __depth, __agg_0 FROM real_pivot
        """,
        out=Csv("""
"price_tier","__depth","__agg_0"
"[NULL]",0,75
2.500000,1,45
1.500000,1,30
        """))
