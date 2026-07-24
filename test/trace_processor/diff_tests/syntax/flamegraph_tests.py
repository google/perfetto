#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


def flamegraph_query(frames_sql, config, cols, props=(), layout=False):
  """Builds a query calling the flamegraph intrinsics over literal frames.

  frames_sql: SQL producing id, parentId, name, value and property columns.
  config: argument list for __intrinsic_flamegraph_config.
  cols: output columns to select, in order.
  props: property columns to pass to __intrinsic_flamegraph_agg.
  layout: lay the tree out with the flamegraph_layout stdlib macro, which
      adds parentCumulativeValue, xStart and xEnd and orders rows for
      rendering.
  """
  agg_args = 'f.id, f.parentId, f.name, f.value'
  for p in props:
    agg_args += f", '{p}', f.{p}"
  ptr = f"""__intrinsic_table_ptr((
          select __intrinsic_flamegraph(
            __intrinsic_flamegraph_agg({agg_args}),
            __intrinsic_flamegraph_config({config}))
          from ({frames_sql}) f
        ))"""
  if not layout:
    sel = ', '.join(f'c{i} as {name}' for i, name in enumerate(cols))
    binds = '\n          and '.join(
        f"__intrinsic_table_ptr_bind(c{i}, '{name}')"
        for i, name in enumerate(cols))
    return f"""
        select {sel}
        from {ptr}
        where {binds};
      """
  base = [
      'id', 'parentId', 'depth', 'name', 'selfValue', 'cumulativeValue',
      'parentCumulativeValue', 'matchedSelfValue', 'ancestorMatchedSelfValue'
  ] + list(props)
  isel = ', '.join(f'c{i} as {name}' for i, name in enumerate(base))
  ibinds = '\n            and '.join(
      f"__intrinsic_table_ptr_bind(c{i}, '{name}')"
      for i, name in enumerate(base))
  sel = ', '.join(cols)
  return f"""
        include perfetto module graphs.flamegraph;
        select {sel}
        from _flamegraph_layout!((
          select {isel}
          from {ptr}
          where {ibinds}
        ));
      """


BASIC_COLS = [
    'name', 'depth', 'selfValue', 'cumulativeValue', 'xStart', 'xEnd'
]

CHAIN_FRAMES = """
            select 1 as id, NULL as parentId, 'main' as name, 1 as value
            union all select 2, 1, 'a', 2
            union all select 3, 2, 'b', 4
            union all select 4, 3, 'c', 8
"""


class PerfettoFlamegraph(TestSuite):

  def test_top_down(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=flamegraph_query(
            """
            select 1 as id, NULL as parentId, 'main' as name, 1 as value
            union all select 2, 1, 'a', 2
            union all select 3, 1, 'a', 3
            union all select 4, 2, 'b', 4
            """, "'view', 'TOP_DOWN'", BASIC_COLS, layout=True),
        out=Csv("""
        "name","depth","selfValue","cumulativeValue","xStart","xEnd"
        "main",1,1,10,0,10
        "a",2,5,9,0,9
        "b",3,4,4,0,4
      """))

  def test_bottom_up(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=flamegraph_query(
            """
            select 1 as id, NULL as parentId, 'main' as name, 1 as value
            union all select 2, 1, 'a', 2
            union all select 3, 2, 'b', 4
            """, "'view', 'BOTTOM_UP'", BASIC_COLS, layout=True),
        out=Csv("""
        "name","depth","selfValue","cumulativeValue","xStart","xEnd"
        "b",-1,4,4,0,4
        "a",-1,2,2,4,6
        "main",-1,1,1,6,7
        "a",-2,2,4,0,4
        "main",-2,1,2,4,6
        "main",-3,1,4,0,4
      """))

  def test_pivot(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=flamegraph_query(CHAIN_FRAMES, "'view', 'PIVOT', 'pivot', '^b$'",
                               BASIC_COLS, layout=True),
        out=Csv("""
        "name","depth","selfValue","cumulativeValue","xStart","xEnd"
        "b",1,4,12,0,12
        "b",-1,4,12,0,12
        "c",2,8,8,0,8
        "a",-2,2,12,0,12
        "main",-3,1,12,0,12
      """))

  def test_show_stack_filter(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=flamegraph_query(
            """
            select 1 as id, NULL as parentId, 'main' as name, 1 as value
            union all select 2, 1, 'a', 2
            union all select 3, 2, 'c', 4
            union all select 4, 1, 'b', 8
            """, "'view', 'TOP_DOWN', 'filter', 'SHOW_STACK', 'c'",
            BASIC_COLS, layout=True),
        out=Csv("""
        "name","depth","selfValue","cumulativeValue","xStart","xEnd"
        "main",1,1,4,0,4
        "a",2,2,4,0,4
        "c",3,4,4,0,4
      """))

  def test_hide_stack_filter(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=flamegraph_query(
            """
            select 1 as id, NULL as parentId, 'main' as name, 1 as value
            union all select 2, 1, 'a', 2
            union all select 3, 2, 'c', 4
            union all select 4, 1, 'b', 8
            """, "'view', 'TOP_DOWN', 'filter', 'HIDE_STACK', 'c'",
            BASIC_COLS, layout=True),
        out=Csv("""
        "name","depth","selfValue","cumulativeValue","xStart","xEnd"
        "main",1,1,11,0,11
        "b",2,8,8,0,8
        "a",2,2,2,8,10
      """))

  def test_show_from_frame_filter(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=flamegraph_query(
            CHAIN_FRAMES, "'view', 'TOP_DOWN', 'filter', 'SHOW_FROM_FRAME', 'b'",
            BASIC_COLS, layout=True),
        out=Csv("""
        "name","depth","selfValue","cumulativeValue","xStart","xEnd"
        "b",1,4,12,0,12
        "c",2,8,8,0,8
      """))

  def test_hide_frame_filter(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=flamegraph_query(
            """
            select 1 as id, NULL as parentId, 'main' as name, 1 as value
            union all select 2, 1, 'skipme', 2
            union all select 3, 2, 'c', 3
            """, "'view', 'TOP_DOWN', 'filter', 'HIDE_FRAME', 'skipme'",
            BASIC_COLS, layout=True),
        out=Csv("""
        "name","depth","selfValue","cumulativeValue","xStart","xEnd"
        "main",1,3,6,0,6
        "c",2,3,3,0,3
      """))

  def test_grouping_column(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=flamegraph_query(
            """
            select 1 as id, NULL as parentId, 'r' as name, 1 as value,
                   NULL as g
            union all select 2, 1, 'x', 2, 'm1'
            union all select 3, 1, 'x', 3, 'm2'
            """, "'view', 'TOP_DOWN', 'grouping', 'g'",
            ['name', 'g', 'cumulativeValue', 'xStart', 'xEnd'],
            props=['g'],
            layout=True),
        out=Csv("""
        "name","g","cumulativeValue","xStart","xEnd"
        "r","[NULL]",6,0,6
        "x","m2",3,0,3
        "x","m1",2,3,5
      """))

  def test_aggregate_columns(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=flamegraph_query(
            """
            select 1 as id, NULL as parentId, 'r' as name, 1 as value,
                   'z' as c, NULL as s, 'f0' as f
            union all select 2, 1, 'x', 2, 'p', 3, 'f1'
            union all select 3, 1, 'x', 3, 'q', 5, 'f2'
            """, """
                'view', 'TOP_DOWN',
                'aggregate', 'f', 'ONE_OR_SUMMARY',
                'aggregate', 's', 'SUM',
                'aggregate', 'c', 'CONCAT_WITH_COMMA'
            """, ['name', 'f', 's', 'c'],
            props=['c', 's', 'f']),
        out=Csv("""
        "name","f","s","c"
        "r","f0","[NULL]","z"
        "x","f1  and 2 others",8,"p,q"
      """))

  def test_empty_input(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query=flamegraph_query(
            """
            select 0 as id, NULL as parentId, '' as name, 0 as value,
                   '' as g
            where 0
            """, "'view', 'TOP_DOWN', 'grouping', 'g'",
            ['name', 'depth', 'g'],
            props=['g']),
        out=Csv("""
        "name","depth","g"
      """))
