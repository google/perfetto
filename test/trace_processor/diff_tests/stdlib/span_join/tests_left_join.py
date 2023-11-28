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


class SpanJoinLeftJoin(TestSuite):

  def test_span_left_join(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query=Path('span_left_join_test.sql'),
        out=Path('span_left_join.out'))

  def test_span_left_join_unpartitioned(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query=Path('span_left_join_unpartitioned_test.sql'),
        out=Path('span_left_join_unpartitioned.out'))

  def test_span_left_join_left_unpartitioned(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query=Path('span_left_join_left_unpartitioned_test.sql'),
        out=Path('span_left_join_left_unpartitioned.out'))

  def test_span_left_join_left_partitioned(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query=Path('span_left_join_left_partitioned_test.sql'),
        out=Path('span_left_join_left_partitioned.out'))

  def test_span_left_join_empty_right(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query="""
        CREATE TABLE t1(
          ts BIGINT,
          dur BIGINT,
          part BIGINT,
          PRIMARY KEY (part, ts)
        ) WITHOUT ROWID;

        CREATE TABLE t2(
          ts BIGINT,
          dur BIGINT,
          part BIGINT,
          PRIMARY KEY (part, ts)
        ) WITHOUT ROWID;

        INSERT INTO t1(ts, dur, part)
        VALUES
        (500, 500, 100);

        CREATE VIRTUAL TABLE sp USING span_left_join(t1 PARTITIONED part,
                                                     t2 PARTITIONED part);

        SELECT * FROM sp;
        """,
        out=Csv("""
        "ts","dur","part"
        500,500,100
        """))

  def test_span_left_join_unordered_android_sched_and_ps(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query="""
        CREATE TABLE t1(
          ts BIGINT,
          dur BIGINT,
          part BIGINT,
          PRIMARY KEY (part, ts)
        ) WITHOUT ROWID;

        CREATE TABLE t2(
          ts BIGINT,
          dur BIGINT,
          part BIGINT,
          PRIMARY KEY (part, ts)
        ) WITHOUT ROWID;

        INSERT INTO t1(ts, dur, part)
        VALUES (500, 100, 10);

        INSERT INTO t2(ts, dur, part)
        VALUES (500, 100, 5);

        CREATE VIRTUAL TABLE sp USING span_left_join(t1 PARTITIONED part,
                                                     t2 PARTITIONED part);

        SELECT * FROM sp;
        """,
        out=Csv("""
        "ts","dur","part"
        500,100,10
        """))
