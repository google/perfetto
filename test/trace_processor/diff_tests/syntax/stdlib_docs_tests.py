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

  def test_stdlib_objects_modules(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT module, package
        FROM __intrinsic_stdlib_objects
        WHERE object_type = 'MODULE'
          AND module IN (
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

  def test_stdlib_objects_all_kinds(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT object_type, COUNT(*) > 0 AS present
        FROM __intrinsic_stdlib_objects
        WHERE object_type IN ('MODULE', 'TABLE', 'VIEW', 'FUNCTION',
                              'TABLE_FUNCTION', 'MACRO')
        GROUP BY object_type
        ORDER BY object_type;
        """,
        out=Csv("""
        "object_type","present"
        "FUNCTION",1
        "MACRO",1
        "MODULE",1
        "TABLE",1
        "TABLE_FUNCTION",1
        "VIEW",1
        """))

  def test_stdlib_objects_self_join(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT module.qualified_name, function.qualified_name
        FROM __intrinsic_stdlib_objects AS module
        JOIN __intrinsic_stdlib_objects AS function
          ON function.module = module.module
        WHERE module.qualified_name = 'time.conversion'
          AND function.qualified_name = 'time.conversion.time_from_ns';
        """,
        out=Csv("""
        "qualified_name","qualified_name"
        "time.conversion","time.conversion.time_from_ns"
        """))

  def test_stdlib_objects_table(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT qualified_name, object_type, exposed, short_description
        FROM __intrinsic_stdlib_objects
        WHERE module = 'slices.with_context'
          AND name = 'thread_slice';
        """,
        out=Csv("""
        "qualified_name","object_type","exposed","short_description"
        "slices.with_context.thread_slice","VIEW",1,"All thread slices with data about thread, thread track and process."
        """))

  def test_stdlib_objects_internal_table(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT name, object_type, exposed
        FROM __intrinsic_stdlib_objects
        WHERE module = 'slices.flat_slices'
          AND name = '_slice_flattened';
        """,
        out=Csv("""
        "name","object_type","exposed"
        "_slice_flattened","TABLE",0
        """))

  def test_stdlib_objects_columns(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT
          c.value ->> 'name' AS col_name,
          c.value ->> 'type' AS col_type,
          c.value ->> 'description' AS col_description
        FROM __intrinsic_stdlib_objects AS o,
             json_each(o.cols) AS c
        WHERE o.qualified_name = 'slices.with_context.thread_slice'
          AND c.value ->> 'name' = 'tid';
        """,
        out=Csv("""
        "col_name","col_type","col_description"
        "tid","LONG","Alias for `thread.tid`."
        """))

  def test_stdlib_objects_function(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT
          o.object_type,
          o.return_type,
          o.return_description,
          a.value ->> 'name' AS arg_name,
          a.value ->> 'type' AS arg_type
        FROM __intrinsic_stdlib_objects AS o,
             json_each(o.args) AS a
        WHERE o.qualified_name = 'time.conversion.time_from_ns';
        """,
        out=Csv("""
        "object_type","return_type","return_description","arg_name","arg_type"
        "FUNCTION","TIMESTAMP","Time duration in nanoseconds.","nanos","LONG"
        """))

  def test_stdlib_objects_table_function(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT object_type, return_type, exposed
        FROM __intrinsic_stdlib_objects
        WHERE qualified_name =
          'slices.hierarchy._slice_ancestor_and_self';
        """,
        out=Csv("""
        "object_type","return_type","exposed"
        "TABLE_FUNCTION","TABLE",0
        """))

  def test_stdlib_objects_macro(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT
          o.return_type,
          o.exposed,
          a.value ->> 'name' AS arg_name,
          a.value ->> 'type' AS arg_type
        FROM __intrinsic_stdlib_objects AS o,
             json_each(o.args) AS a
        WHERE o.qualified_name = 'intervals.intersect._ii_df_agg'
        ORDER BY arg_name;
        """,
        out=Csv("""
        "return_type","exposed","arg_name","arg_type"
        "_ProjectionFragment",0,"x","ColumnName"
        "_ProjectionFragment",0,"y","ColumnName"
        """))

  def test_stdlib_objects_summary(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT
          summary GLOB '*Kind: public FUNCTION*' AS has_kind,
          summary GLOB '*Arguments:*nanos (LONG)*' AS has_arg,
          summary GLOB '*Returns: TIMESTAMP*' AS has_return,
          summary GLOB '*Search aliases: time time conversion time from ns*'
            AS has_alias
        FROM __intrinsic_stdlib_objects
        WHERE qualified_name = 'time.conversion.time_from_ns';
        """,
        out=Csv("""
        "has_kind","has_arg","has_return","has_alias"
        1,1,1,1
        """))

  def test_stdlib_objects_case_insensitive_search(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT qualified_name
        FROM __intrinsic_stdlib_objects
        WHERE exposed = 1
          AND regexp('TIME.TO.MS|MILLISECONDS', summary, 'i')
          AND qualified_name = 'time.conversion.time_to_ms';
        """,
        out=Csv("""
        "qualified_name"
        "time.conversion.time_to_ms"
        """))
