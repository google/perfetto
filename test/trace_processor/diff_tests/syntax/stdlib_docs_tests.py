#!/usr/bin/env python3
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

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class StdlibDocs(TestSuite):

  def test_stdlib_modules_slices(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT module, package
        FROM __intrinsic_stdlib_modules()
        WHERE module IN (
          'slices.with_context',
          'slices.flat_slices',
          'slices.hierarchy'
        )
        ORDER BY module;
        """,
        out=Csv("""
        "module","package"
        "slices.flat_slices","slices"
        "slices.hierarchy","slices"
        "slices.with_context","slices"
        """))

  def test_stdlib_modules_nonempty(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT COUNT(*) > 0 AS has_modules,
               COUNT(DISTINCT package) > 0 AS has_packages
        FROM __intrinsic_stdlib_modules();
        """,
        out=Csv("""
        "has_modules","has_packages"
        1,1
        """))

  def test_stdlib_tables_slices_with_context(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT name, type, exposed
        FROM __intrinsic_stdlib_tables
        WHERE module = 'slices.with_context'
        ORDER BY name;
        """,
        out=Csv("""
        "name","type","exposed"
        "process_slice","VIEW",1
        "thread_or_process_slice","VIEW",1
        "thread_slice","VIEW",1
        """))

  def test_stdlib_tables_include_internal(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT name, type, exposed
        FROM __intrinsic_stdlib_tables
        WHERE module = 'slices.flat_slices'
          AND name = '_slice_flattened';
        """,
        out=Csv("""
        "name","type","exposed"
        "_slice_flattened","TABLE",0
        """))

  def test_stdlib_tables_columns(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT
          c.value ->> 'name' AS col_name,
          c.value ->> 'type' AS col_type
        FROM __intrinsic_stdlib_tables t,
             json_each(t.cols) c
        WHERE t.module = 'slices.with_context'
          AND t.name = 'thread_slice'
        LIMIT 4;
        """,
        out=Csv("""
        "col_name","col_type"
        "id","ID(slice.id)"
        "ts","TIMESTAMP"
        "dur","DURATION"
        "category","STRING"
        """))

  def test_stdlib_tables_column_description(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT
          c.value ->> 'name' AS col_name,
          c.value ->> 'description' AS col_desc
        FROM __intrinsic_stdlib_tables t,
             json_each(t.cols) c
        WHERE t.module = 'slices.with_context'
          AND t.name = 'thread_slice'
          AND c.value ->> 'name' = 'tid';
        """,
        out=Csv("""
        "col_name","col_desc"
        "tid","Alias for `thread.tid`."
        """))

  def test_stdlib_tables_description(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT name, description
        FROM __intrinsic_stdlib_tables
        WHERE module = 'slices.with_context'
          AND name = 'thread_slice';
        """,
        out=Csv("""
        "name","description"
        "thread_slice","All thread slices with data about thread, thread track and process."
        """))

  def test_stdlib_functions_scalar(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT name, is_table_function, return_type, exposed
        FROM __intrinsic_stdlib_functions
        WHERE module = 'time.conversion'
          AND name = 'time_from_ns';
        """,
        out=Csv("""
        "name","is_table_function","return_type","exposed"
        "time_from_ns",0,"TIMESTAMP",1
        """))

  def test_stdlib_functions_args(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT
          f.name,
          a.value ->> 'name' AS arg_name,
          a.value ->> 'type' AS arg_type,
          a.value ->> 'description' AS arg_desc
        FROM (SELECT * FROM __intrinsic_stdlib_functions
              WHERE module = 'time.conversion') f,
             json_each(f.args) a
        WHERE f.name = 'time_from_ns';
        """,
        out=Csv("""
        "name","arg_name","arg_type","arg_desc"
        "time_from_ns","nanos","LONG","Time duration in nanoseconds."
        """))

  def test_stdlib_functions_internal_table_function(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT name, is_table_function, return_type, exposed
        FROM __intrinsic_stdlib_functions
        WHERE module = 'slices.hierarchy'
          AND name = '_slice_ancestor_and_self';
        """,
        out=Csv("""
        "name","is_table_function","return_type","exposed"
        "_slice_ancestor_and_self",1,"TABLE",0
        """))

  def test_stdlib_macros(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT name, return_type, exposed
        FROM __intrinsic_stdlib_macros
        WHERE module = 'intervals.intersect'
          AND name = '_ii_df_agg';
        """,
        out=Csv("""
        "name","return_type","exposed"
        "_ii_df_agg","_ProjectionFragment",0
        """))

  def test_stdlib_macro_args(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT
          m.name,
          a.value ->> 'name' AS arg_name,
          a.value ->> 'type' AS arg_type
        FROM (SELECT * FROM __intrinsic_stdlib_macros
              WHERE module = 'intervals.intersect') m,
             json_each(m.args) a
        WHERE m.name = '_ii_df_agg'
        ORDER BY arg_name;
        """,
        out=Csv("""
        "name","arg_name","arg_type"
        "_ii_df_agg","x","ColumnName"
        "_ii_df_agg","y","ColumnName"
        """))

  def test_stdlib_tables_description_non_first_stmt(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT name, description
        FROM __intrinsic_stdlib_tables
        WHERE module = 'slices.with_context'
          AND name = 'process_slice';
        """,
        out=Csv("""
        "name","description"
        "process_slice","All process slices with data about process track and process."
        """))

  def test_stdlib_tables_column_description_non_first_stmt(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT
          c.value ->> 'name' AS col_name,
          c.value ->> 'description' AS col_desc
        FROM __intrinsic_stdlib_tables t,
             json_each(t.cols) c
        WHERE t.module = 'slices.with_context'
          AND t.name = 'process_slice'
          AND c.value ->> 'name' = 'pid';
        """,
        out=Csv("""
        "col_name","col_desc"
        "pid","Alias for `process.pid`."
        """))

  def test_stdlib_functions_return_description(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT name, return_description
        FROM __intrinsic_stdlib_functions
        WHERE module = 'time.conversion'
          AND name = 'time_from_ns';
        """,
        out=Csv("""
        "name","return_description"
        "time_from_ns","Time duration in nanoseconds."
        """))

  def test_stdlib_functions_return_description_non_first_stmt(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT name, return_description
        FROM __intrinsic_stdlib_functions
        WHERE module = 'time.conversion'
          AND name = 'time_from_us';
        """,
        out=Csv("""
        "name","return_description"
        "time_from_us","Time duration in nanoseconds."
        """))

  def test_stdlib_functions_args_non_first_stmt(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT
          f.name,
          a.value ->> 'name' AS arg_name,
          a.value ->> 'type' AS arg_type,
          a.value ->> 'description' AS arg_desc
        FROM (SELECT * FROM __intrinsic_stdlib_functions
              WHERE module = 'time.conversion') f,
             json_each(f.args) a
        WHERE f.name = 'time_from_us';
        """,
        out=Csv("""
        "name","arg_name","arg_type","arg_desc"
        "time_from_us","micros","LONG","Time duration in microseconds."
        """))

  def test_stdlib_tables_null_module(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT COUNT(*) AS c FROM __intrinsic_stdlib_tables(NULL);
        """,
        out=Csv("""
        "c"
        0
        """))

  def test_stdlib_functions_null_module(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT COUNT(*) AS c FROM __intrinsic_stdlib_functions(NULL);
        """,
        out=Csv("""
        "c"
        0
        """))

  def test_stdlib_macros_null_module(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT COUNT(*) AS c FROM __intrinsic_stdlib_macros(NULL);
        """,
        out=Csv("""
        "c"
        0
        """))
