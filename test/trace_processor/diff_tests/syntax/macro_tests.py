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


class PerfettoMacro(TestSuite):

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

  def test_replace_macro(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query='''
        CREATE PERFETTO MACRO foo() RETURNS Expr AS 1;
        CREATE OR REPLACE PERFETTO MACRO foo() RETURNS Expr AS 2;

        SELECT foo!() as res;
        ''',
        out=Csv("""
        "res"
        2
        """))

  def test_stringify(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query='''
        SELECT __intrinsic_stringify!(foo)
        UNION ALL
        SELECT __intrinsic_stringify!(foo bar baz)
        UNION ALL
        SELECT __intrinsic_stringify!(foo'')
        UNION ALL
        SELECT __intrinsic_stringify!(bar())
        ''',
        out=Csv("""
        "'foo'"
        "foo"
        "foo bar baz"
        "foo'"
        "bar()"
        """))
