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

  def test_distinct_multi_column(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        CREATE PERFETTO TABLE foo AS
        WITH data(a, b) AS (
          VALUES
            -- Needed to defeat any id/sorted detection.
            (2, 3),
            (0, 2),
            (0, 1)
        )
        SELECT * FROM data;

        CREATE TABLE bar AS
        SELECT 1 AS b;

        WITH multi_col_distinct AS (
          SELECT DISTINCT a FROM foo CROSS JOIN bar USING (b)
        ), multi_col_group_by AS (
          SELECT a FROM foo CROSS JOIN bar USING (b) GROUP BY a
        )
        SELECT
          (SELECT COUNT(*) FROM multi_col_distinct) AS cnt_distinct,
          (SELECT COUNT(*) FROM multi_col_group_by) AS cnt_group_by
        """,
        out=Csv("""
        "cnt_distinct","cnt_group_by"
        1,1
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

  def test_create_perfetto_index(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO INDEX foo ON __intrinsic_slice(track_id);
        CREATE PERFETTO INDEX foo_name ON __intrinsic_slice(name);

        SELECT
          COUNT() FILTER (WHERE track_id > 10) AS track_idx,
          COUNT() FILTER (WHERE name > "g") AS name_idx
        FROM __intrinsic_slice;
        """,
        out=Csv("""
        "track_idx","name_idx"
        20717,7098
        """))

  def test_create_perfetto_index_multiple_cols(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO INDEX foo ON __intrinsic_slice(track_id, name);

        SELECT
          MIN(track_id) AS min_track_id,
          MAX(name) AS min_name
        FROM __intrinsic_slice
        WHERE track_id = 13 AND name > "c"
        """,
        out=Csv("""
        "min_track_id","min_name"
        13,"virtual bool art::ElfOatFile::Load(const std::string &, bool, bool, bool, art::MemMap *, std::string *)"
        """))

  def test_create_perfetto_index_multiple_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO INDEX idx ON __intrinsic_slice(track_id, name);
        CREATE PERFETTO TABLE bar AS SELECT * FROM slice;

       SELECT (
          SELECT count()
          FROM bar
          WHERE track_id = 13 AND dur > 1000 AND name > "b"
        ) AS non_indexes_stats,
        (
          SELECT count()
          FROM slice
          WHERE track_id = 13 AND dur > 1000 AND name > "b"
        ) AS indexed_stats
        """,
        out=Csv("""
        "non_indexes_stats","indexed_stats"
        39,39
        """))

  def test_create_or_replace_perfetto_index(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO INDEX idx ON __intrinsic_slice(track_id, name);
        CREATE OR REPLACE PERFETTO INDEX idx ON __intrinsic_slice(name);

       SELECT MAX(id) FROM slice WHERE track_id = 13;
        """,
        out=Csv("""
        "MAX(id)"
        20745
        """))

  def test_create_or_replace_perfetto_index(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO INDEX idx ON __intrinsic_slice(track_id, name);
        DROP PERFETTO INDEX idx ON __intrinsic_slice;

        SELECT MAX(id) FROM slice WHERE track_id = 13;
        """,
        out=Csv("""
        "MAX(id)"
        20745
        """))
