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
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from python.generators.diff_tests.testing import PrintProfileProto
from google.protobuf import text_format


class PreludeMathFunctions(TestSuite):

  def test_math_ln_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT
          CAST(LN(1) * 1000 AS INTEGER) AS valid,
          LN("as") AS invalid_str,
          LN(NULL) AS invalid_null
        """,
        out=Csv("""
        "valid","invalid_str","invalid_null"
        0,"[NULL]","[NULL]"
        """))

  def test_math_trunc_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT
          TRUNC(5.5) as trunc_positive,
          TRUNC(-5.5) as trunc_negative,
          TRUNC(5) as trunc_integer,
          TRUNC(NULL) as trunc_null
        """,
        out=Csv("""
        "trunc_positive","trunc_negative","trunc_integer","trunc_null"
        5.000000,-5.000000,5,"[NULL]"
        """))

  def test_math_ceil_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT
          CEIL(5.5) as ceil_positive,
          CEIL(-5.5) as ceil_negative,
          CEIL(5) as ceil_integer,
          CEIL(NULL) as ceil_null
        """,
        out=Csv("""
        "ceil_positive","ceil_negative","ceil_integer","ceil_null"
        6.000000,-5.000000,5,"[NULL]"
        """))

  def test_math_pow_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT
          POW(2, 3) as pow_int,
          POW(2.5, 2) as pow_float,
          POW(4, 0.5) as pow_root,
          POW(NULL, 2) as pow_null_base,
          POW(2, NULL) as pow_null_exponent
        """,
        out=Csv("""
        "pow_int","pow_float","pow_root","pow_null_base","pow_null_exponent"
        8.000000,6.250000,2.000000,"[NULL]","[NULL]"
        """))

  def test_math_exp_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT
          CAST(EXP(1) * 1000 AS INTEGER) AS valid,
          EXP("asd") AS invalid_str,
          EXP(NULL) AS invalid_null
        """,
        out=Csv("""
        "valid","invalid_str","invalid_null"
        2718,"[NULL]","[NULL]"
        """))

  def test_math_sqrt_function(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT
          CAST(SQRT(4) AS INTEGER) AS valid,
          SQRT("asd") AS invalid_str,
          SQRT(NULL) AS invalid_null
        """,
        out=Csv("""
        "valid","invalid_str","invalid_null"
        2,"[NULL]","[NULL]"
        """))

  def test_math_functions(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        SELECT
          CAST(SQRT(EXP(LN(1))) AS INTEGER) AS valid,
          SQRT(EXP(LN("asd"))) AS invalid_str,
          SQRT(EXP(LN(NULL))) AS invalid_null
        """,
        out=Csv("""
        "valid","invalid_str","invalid_null"
        1,"[NULL]","[NULL]"
        """))
