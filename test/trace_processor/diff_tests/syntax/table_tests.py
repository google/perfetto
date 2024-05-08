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


class PerfettoTable(TestSuite):

  def test_create_table(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        CREATE PERFETTO TABLE foo AS SELECT 42 as a;

        SELECT * FROM foo;
        """,
        out=Csv("""
        "a"
        42
        """))

  def test_replace_table(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        CREATE PERFETTO TABLE foo AS SELECT 42 as a;
        CREATE OR REPLACE PERFETTO TABLE foo AS SELECT 43 as a;

        SELECT * FROM foo;
        """,
        out=Csv("""
        "a"
        43
        """))

  def test_create_perfetto_table_double_metric_run(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT RUN_METRIC('android/cpu_info.sql');
        SELECT RUN_METRIC('android/cpu_info.sql');

        SELECT * FROM cluster_core_type;
        """,
        out=Csv("""
        "cluster","core_type"
        0,"little"
        1,"big"
        2,"bigger"
        """))

  def test_perfetto_table_info_static_table(self):
    return DiffTestBlueprint(
        trace=DataPath('android_boot.pftrace'),
        query="""
        SELECT * FROM perfetto_table_info('counter');
        """,
        out=Csv("""
        "id","type","name","col_type","nullable","sorted"
        0,"perfetto_table_info","id","id",0,1
        1,"perfetto_table_info","type","string",0,0
        2,"perfetto_table_info","ts","int64",0,1
        3,"perfetto_table_info","track_id","uint32",0,0
        4,"perfetto_table_info","value","double",0,0
        5,"perfetto_table_info","arg_set_id","uint32",1,0
        6,"perfetto_table_info","machine_id","uint32",1,0
        """))

  def test_perfetto_table_info_runtime_table(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        CREATE PERFETTO TABLE foo AS
        SELECT * FROM
        (SELECT 2 AS c
        UNION
        SELECT 0 AS c
        UNION
        SELECT 1 AS c)
        ORDER BY c desc;

        SELECT * from perfetto_table_info('foo');
        """,
        out=Csv("""
        "id","type","name","col_type","nullable","sorted"
        0,"perfetto_table_info","c","int64",0,0
        """))

  def test_create_perfetto_table_nullable_column(self):
    return DiffTestBlueprint(
        trace=DataPath('android_boot.pftrace'),
        query="""
        CREATE PERFETTO TABLE foo AS
        SELECT thread_ts FROM slice
        WHERE thread_ts IS NOT NULL;

        SELECT nullable FROM perfetto_table_info('foo')
        WHERE name = 'thread_ts';
        """,
        out=Csv("""
        "nullable"
        0
        """))

  def test_create_perfetto_table_nullable_column(self):
    return DiffTestBlueprint(
        trace=DataPath('android_boot.pftrace'),
        query="""
        CREATE PERFETTO TABLE foo AS
        SELECT dur FROM slice ORDER BY dur;

        SELECT sorted FROM perfetto_table_info('foo')
        WHERE name = 'dur';
        """,
        out=Csv("""
        "sorted"
        1
        """))

  def test_create_perfetto_table_id_column(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        CREATE PERFETTO TABLE foo AS
        SELECT 2 AS c
        UNION
        SELECT 4
        UNION
        SELECT 6;

        SELECT col_type FROM perfetto_table_info('foo')
        WHERE name = 'c';
        """,
        out=Csv("""
        "col_type"
        "id"
        """))

  def test_distinct_trivial(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        WITH trivial_count AS (
          SELECT DISTINCT name FROM slice
        ),
        few_results AS (
          SELECT DISTINCT depth FROM slice
        ),
        simple_nullable AS (
          SELECT DISTINCT parent_id FROM slice
        ),
        selector AS (
          SELECT DISTINCT cpu FROM ftrace_event
        )
        SELECT
          (SELECT COUNT(*) FROM trivial_count) AS name,
          (SELECT COUNT(*) FROM few_results) AS depth,
          (SELECT COUNT(*) FROM simple_nullable) AS parent_id,
          (SELECT COUNT(*) FROM selector) AS cpu_from_ftrace;
        """,
        out=Csv("""
        "name","depth","parent_id","cpu_from_ftrace"
        3073,8,4529,8
        """))

  def test_limit(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        WITH data(a, b) AS (
          VALUES
            (0, 1),
            (1, 10),
            (2, 20),
            (3, 30),
            (4, 40),
            (5, 50)
        )
        SELECT * FROM data LIMIT 3;
        """,
        out=Csv("""
        "a","b"
        0,1
        1,10
        2,20
        """))

  def test_limit_and_offset_in_bounds(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        WITH data(a, b) AS (
          VALUES
            (0, 1),
            (1, 10),
            (2, 20),
            (3, 30),
            (4, 40),
            (5, 50),
            (6, 60),
            (7, 70),
            (8, 80),
            (9, 90)
        )
        SELECT * FROM data LIMIT 2 OFFSET 3;
        """,
        out=Csv("""
        "a","b"
        3,30
        4,40
        """))

  def test_limit_and_offset_not_in_bounds(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        WITH data(a, b) AS (
          VALUES
            (0, 1),
            (1, 10),
            (2, 20),
            (3, 30),
            (4, 40),
            (5, 50),
            (6, 60),
            (7, 70),
            (8, 80),
            (9, 90)
        )
        SELECT * FROM data LIMIT 5 OFFSET 6;
        """,
        out=Csv("""
        "a","b"
        6,60
        7,70
        8,80
        9,90
        """))

  def test_max(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO MACRO max(col ColumnName)
        RETURNS TableOrSubquery AS (
          SELECT id, $col
          FROM slice
          ORDER BY $col DESC
          LIMIT 1
        );

        SELECT
          (SELECT id FROM max!(id)) AS id,
          (SELECT id FROM max!(dur)) AS numeric,
          (SELECT id FROM max!(name)) AS string,
          (SELECT id FROM max!(parent_id)) AS nullable;
        """,
        out=Csv("""
        "id","numeric","string","nullable"
        20745,2698,148,20729
        """))

  def test_min(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO MACRO min(col ColumnName)
        RETURNS TableOrSubquery AS (
          SELECT id, $col
          FROM slice
          ORDER BY $col ASC
          LIMIT 1
        );

        SELECT
          (SELECT id FROM min!(id)) AS id,
          (SELECT id FROM min!(dur)) AS numeric,
          (SELECT id FROM min!(name)) AS string,
          (SELECT id FROM min!(parent_id)) AS nullable;
        """,
        out=Csv("""
        "id","numeric","string","nullable"
        0,3111,460,0
        """))
