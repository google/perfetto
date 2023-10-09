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


class PerfettoSql(TestSuite):

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

  def test_import(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 1
            event {
              timestamp: 1000
              pid: 1
              print {
                buf: "C|1000|battery_stats.data_conn|13\n"
              }
            }
            event {
              timestamp: 4000
              pid: 1
              print {
                buf: "C|1000|battery_stats.data_conn|20\n"
              }
            }
            event {
              timestamp: 1000
              pid: 1
              print {
                buf: "C|1000|battery_stats.audio|1\n"
              }
            }
          }
        }
        """),
        query="""
        SELECT IMPORT('common.timestamps');

        SELECT TRACE_START();
        """,
        out=Csv("""
        "TRACE_START()"
        1000
        """))

  def test_include_perfetto_module(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 1
            event {
              timestamp: 1000
              pid: 1
              print {
                buf: "C|1000|battery_stats.data_conn|13\n"
              }
            }
            event {
              timestamp: 4000
              pid: 1
              print {
                buf: "C|1000|battery_stats.data_conn|20\n"
              }
            }
            event {
              timestamp: 1000
              pid: 1
              print {
                buf: "C|1000|battery_stats.audio|1\n"
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE common.timestamps;

        SELECT TRACE_START();
        """,
        out=Csv("""
        "TRACE_START()"
        1000
        """))

  def test_include_and_import(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 1
            event {
              timestamp: 1000
              pid: 1
              print {
                buf: "C|1000|battery_stats.data_conn|13\n"
              }
            }
            event {
              timestamp: 4000
              pid: 1
              print {
                buf: "C|1000|battery_stats.data_conn|20\n"
              }
            }
            event {
              timestamp: 1000
              pid: 1
              print {
                buf: "C|1000|battery_stats.audio|1\n"
              }
            }
          }
        }
        """),
        query="""
        SELECT IMPORT('common.timestamps');
        INCLUDE PERFETTO MODULE common.timestamps;

        SELECT TRACE_START();
        """,
        out=Csv("""
        "TRACE_START()"
        1000
        """))

  def test_macro(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query='''
        CREATE PERFETTO MACRO foo(a Expr,b Expr) RETURNS TableOrSubquery AS
        SELECT $a - $b;
        SELECT (foo!(123, 100)) as res;
        ''',
        out=Csv("""
        "res"
        23
        """))

  def test_nested_macro(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query='''
        CREATE PERFETTO MACRO foo(a Expr) returns Expr AS $a;
        CREATE PERFETTO MACRO bar(a Expr) returns Expr AS (SELECT $a);
        CREATE PERFETTO MACRO baz(a Expr,b Expr) returns TableOrSubquery AS
        SELECT bar!(foo!(123)) - $b as res;
        baz!(123, 100);
        ''',
        out=Csv("""
        "res"
        23
        """))
