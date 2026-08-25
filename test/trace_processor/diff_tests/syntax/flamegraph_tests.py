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
from python.generators.diff_tests.testing import ExpectedError
from python.generators.diff_tests.testing import TestSuite


class PerfettoFlamegraph(TestSuite):

  def test_top_down(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'main' AS name, 1 AS value
            UNION ALL SELECT 2, 1, 'a', 2
            UNION ALL SELECT 3, 1, 'a', 3
            UNION ALL SELECT 4, 2, 'b', 4
          ));
          SELECT name, depth, self_value, cumulative_value,
                 parent_cumulative_value, x_start, x_end
          FROM fg(__intrinsic_flamegraph_config(
              'value', 'value', 'view', 'TOP_DOWN'));
        """,
        out=Csv("""
        "name","depth","self_value","cumulative_value","parent_cumulative_value","x_start","x_end"
        "main",1,1,10,"[NULL]",0,10
        "a",2,5,9,10,0,9
        "b",3,4,4,9,0,4
      """))

  def test_bottom_up(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'main' AS name, 1 AS value
            UNION ALL SELECT 2, 1, 'a', 2
            UNION ALL SELECT 3, 2, 'b', 4
          ));
          SELECT name, depth, self_value, cumulative_value, x_start, x_end
          FROM fg(__intrinsic_flamegraph_config(
              'value', 'value', 'view', 'BOTTOM_UP'));
        """,
        out=Csv("""
        "name","depth","self_value","cumulative_value","x_start","x_end"
        "b",-1,4,4,0,4
        "a",-1,2,2,4,6
        "main",-1,1,1,6,7
        "a",-2,0,4,0,4
        "main",-2,0,2,4,6
        "main",-3,0,4,0,4
      """))

  def test_pivot(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'main' AS name, 1 AS value
            UNION ALL SELECT 2, 1, 'a', 2
            UNION ALL SELECT 3, 2, 'b', 4
            UNION ALL SELECT 4, 3, 'c', 8
          ));
          SELECT name, depth, self_value, cumulative_value, x_start, x_end
          FROM fg(__intrinsic_flamegraph_config(
              'value', 'value', 'view', 'PIVOT',
              'view_pattern', '^b$', ''));
        """,
        out=Csv("""
        "name","depth","self_value","cumulative_value","x_start","x_end"
        "b",1,4,12,0,12
        "b",-1,12,12,0,12
        "c",2,8,8,0,8
        "a",-2,0,12,0,12
        "main",-3,0,12,0,12
      """))

  def test_show_stack_filter(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'main' AS name, 1 AS value
            UNION ALL SELECT 2, 1, 'a', 2
            UNION ALL SELECT 3, 2, 'c', 4
            UNION ALL SELECT 4, 1, 'b', 8
          ));
          SELECT name, depth, self_value, cumulative_value, x_start, x_end
          FROM fg(__intrinsic_flamegraph_config(
              'value', 'value', 'view', 'TOP_DOWN',
              'filter', 'SHOW_STACK', 'c', ''));
        """,
        out=Csv("""
        "name","depth","self_value","cumulative_value","x_start","x_end"
        "main",1,0,4,0,4
        "a",2,0,4,0,4
        "c",3,4,4,0,4
      """))

  def test_case_insensitive_filter(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'Main' AS name, 1 AS value
            UNION ALL SELECT 2, 1, 'Child', 2
          ));
          SELECT name, depth, self_value, cumulative_value, x_start, x_end
          FROM fg(__intrinsic_flamegraph_config(
              'value', 'value', 'view', 'TOP_DOWN',
              'filter', 'SHOW_STACK', 'child', 'i'));
        """,
        out=Csv("""
        "name","depth","self_value","cumulative_value","x_start","x_end"
        "Main",1,0,2,0,2
        "Child",2,2,2,0,2
      """))

  def test_hide_stack_filter(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'main' AS name, 1 AS value
            UNION ALL SELECT 2, 1, 'a', 2
            UNION ALL SELECT 3, 2, 'c', 4
            UNION ALL SELECT 4, 1, 'b', 8
          ));
          SELECT name, depth, self_value, cumulative_value, x_start, x_end
          FROM fg(__intrinsic_flamegraph_config(
              'value', 'value', 'view', 'TOP_DOWN',
              'filter', 'HIDE_STACK', 'c', ''));
        """,
        out=Csv("""
        "name","depth","self_value","cumulative_value","x_start","x_end"
        "main",1,1,11,0,11
        "b",2,8,8,0,8
        "a",2,2,2,8,10
      """))

  def test_from_frame(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'main' AS name, 1 AS value
            UNION ALL SELECT 2, 1, 'a', 2
            UNION ALL SELECT 3, 2, 'b', 4
            UNION ALL SELECT 4, 3, 'c', 8
          ));
          SELECT name, depth, self_value, cumulative_value, x_start, x_end
          FROM fg(__intrinsic_flamegraph_config(
              'value', 'value', 'view', 'FROM_FRAME',
              'view_pattern', 'b', ''));
        """,
        out=Csv("""
        "name","depth","self_value","cumulative_value","x_start","x_end"
        "b",1,4,12,0,12
        "c",2,8,8,0,8
      """))

  def test_hide_frame_filter(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'main' AS name, 1 AS value
            UNION ALL SELECT 2, 1, 'skipme', 2
            UNION ALL SELECT 3, 2, 'c', 3
          ));
          SELECT name, depth, self_value, cumulative_value, x_start, x_end
          FROM fg(__intrinsic_flamegraph_config(
              'value', 'value', 'view', 'TOP_DOWN',
              'filter', 'HIDE_FRAME', 'skipme', ''));
        """,
        out=Csv("""
        "name","depth","self_value","cumulative_value","x_start","x_end"
        "main",1,3,6,0,6
        "c",2,3,3,0,3
      """))

  def test_null_grouping_column_does_not_match_empty_string(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'root' AS name, 4 AS value,
                   NULL AS g
          ));
          SELECT name
          FROM fg(__intrinsic_flamegraph_config(
            'value', 'value', 'view', 'TOP_DOWN', 'grouping', 'g',
            'filter', 'SHOW_STACK', '^$', ''));
        """,
        out=Csv("""
        "name"
      """))

  def test_filter_matches_numeric_grouping_column(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'root' AS name, 4 AS value,
                   (-9223372036854775807 - 1) AS g
          ));
          SELECT name, cumulative_value
          FROM fg(__intrinsic_flamegraph_config(
            'value', 'value', 'view', 'TOP_DOWN', 'grouping', 'g',
            'filter', 'SHOW_STACK', '^-9223372036854775808$', ''));
        """,
        out=Csv("""
        "name","cumulative_value"
        "root",4
      """))

  def test_filter_matches_grouping_column(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'root' AS name, 0 AS value,
                   'other' AS g
            UNION ALL SELECT 2, 1, 'child', 4, 'needle'
          ));
          SELECT name, self_value, cumulative_value
          FROM fg(__intrinsic_flamegraph_config(
            'value', 'value', 'view', 'TOP_DOWN', 'grouping', 'g',
            'filter', 'SHOW_STACK', '^needle$', ''));
        """,
        out=Csv("""
        "name","self_value","cumulative_value"
        "root",0,4
        "child",4,4
      """))

  def test_grouping_column(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'r' AS name, 1 AS value,
                   NULL AS g
            UNION ALL SELECT 2, 1, 'x', 2, 'm1'
            UNION ALL SELECT 3, 1, 'x', 3, 'm2'
          ));
          SELECT name, g, cumulative_value, x_start, x_end
          FROM fg(__intrinsic_flamegraph_config(
              'value', 'value', 'view', 'TOP_DOWN', 'grouping', 'g'));
        """,
        out=Csv("""
        "name","g","cumulative_value","x_start","x_end"
        "r","[NULL]",6,0,6
        "x","m2",3,0,3
        "x","m1",2,3,5
      """))

  def test_hide_frame_aggregate_and_operation_order(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'root' AS name, 1 AS value,
                   2 AS s
            UNION ALL SELECT 2, 1, 'skip', 3, 5
            UNION ALL SELECT 3, 2, 'leaf', 7, 11
            UNION ALL SELECT 4, 1, 'blocked', 13, 17
          ));
          SELECT name, self_value, cumulative_value, s
          FROM fg(__intrinsic_flamegraph_config(
            'value', 'value', 'view', 'TOP_DOWN',
            'filter', 'HIDE_FRAME', '^skip$', '',
            'filter', 'HIDE_STACK', '^(skip|blocked)$', '',
            'aggregate', 'SUM', 's', 's'));
        """,
        out=Csv("""
        "name","self_value","cumulative_value","s"
        "root",4,11,7
        "leaf",7,7,11
      """))

  def test_bottom_up_hide_frame_aggregate(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'root' AS name, 1 AS value,
                   2 AS s
            UNION ALL SELECT 2, 1, 'skip', 3, 5
            UNION ALL SELECT 3, 2, 'leaf', 7, 11
          ));
          SELECT name, depth, self_value, cumulative_value, s
          FROM fg(__intrinsic_flamegraph_config(
            'value', 'value', 'view', 'BOTTOM_UP',
            'filter', 'HIDE_FRAME', '^skip$', '',
            'aggregate', 'SUM', 's', 's'));
        """,
        out=Csv("""
        "name","depth","self_value","cumulative_value","s"
        "leaf",-1,7,7,11
        "root",-1,4,4,7
        "root",-2,0,7,7
      """))

  def test_aggregate_columns(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'r' AS name, 1 AS value,
                   NULL AS s, 'root' AS summary, 'root' AS concatenated,
                   5 AS n, 1.5 AS d
            UNION ALL SELECT 2, 1, 'x', 2, 3, 'a', 'a', 7, 2.5
            UNION ALL SELECT 3, 1, 'x', 3, 5, 'b', 'b', 8, 2.5
          ));
          SELECT name, s, summary, concatenated, n, d
          FROM fg(__intrinsic_flamegraph_config(
            'value', 'value', 'view', 'TOP_DOWN',
            'aggregate', 'SUM', 's', 's',
            'aggregate', 'ONE_OR_SUMMARY', 'summary', 'summary',
            'aggregate', 'CONCAT_WITH_COMMA', 'concatenated', 'concatenated',
            'aggregate', 'CONCAT_WITH_COMMA', 'n', 'n',
            'aggregate', 'CONCAT_WITH_COMMA', 'd', 'd'));
        """,
        out=Csv("""
        "name","s","summary","concatenated","n","d"
        "r","[NULL]","root","root","5","1.5"
        "x",8,"a and 2 others","a,b","7,8","2.5,2.5"
      """))

  def test_multiple_values(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'r' AS name, 0 AS value,
                   1 AS bytes
            UNION ALL SELECT 2, 1, 'x', 2, 0
            UNION ALL SELECT 3, 1, 'y', 0, 4
          ));
          SELECT name, self_value, cumulative_value,
                 self_bytes, cumulative_bytes
          FROM fg(__intrinsic_flamegraph_config(
            'value', 'value', 'view', 'TOP_DOWN', 'value', 'bytes'));
        """,
        out=Csv("""
        "name","self_value","cumulative_value","self_bytes","cumulative_bytes"
        "r",0,2,1,5
        "x",2,2,0,0
        "y",0,0,4,4
      """))

  def test_super_root(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'r1' AS name, 1 AS value
            UNION ALL SELECT 2, NULL, 'r2', 2
            UNION ALL SELECT 3, 1, 'child', 4
          ));
          SELECT _tree_parent_id, depth, name, cumulative_value
          FROM fg(__intrinsic_flamegraph_config(
              'value', 'value', 'view', 'TOP_DOWN'))
          WHERE __intrinsic_flamegraph_find(_tree_id, 'SUPER_ROOT');
        """,
        out=Csv("""
        "_tree_parent_id","depth","name","cumulative_value"
        "[NULL]",0,"[NULL]",7.000000
      """))

  def test_empty_input(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 0 AS id, NULL AS parent_id, '' AS name, 0 AS value,
                   '' AS g
            WHERE FALSE
          ));
          SELECT name, depth, g
          FROM fg(__intrinsic_flamegraph_config(
            'value', 'value', 'view', 'TOP_DOWN', 'grouping', 'g'));
        """,
        out=Csv("""
        "name","depth","g"
      """))

  def test_negative_value(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
          CREATE VIRTUAL TABLE fg USING __intrinsic_flamegraph((
            SELECT 1 AS id, NULL AS parent_id, 'main' AS name, -1 AS value
          ));
          SELECT name, cumulative_value
          FROM fg(__intrinsic_flamegraph_config(
              'value', 'value', 'view', 'TOP_DOWN'));
        """,
        out=ExpectedError('flamegraph: value columns must be non-negative'))
