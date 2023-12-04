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


class SpanJoinOuterJoin(TestSuite):

  def test_span_outer_join(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query=Path('span_outer_join_test.sql'),
        out=Path('span_outer_join.out'))

  def test_span_outer_join_empty(self):
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


        CREATE VIRTUAL TABLE sp USING span_outer_join(t1 PARTITIONED part,
                                                      t2 PARTITIONED part);

        SELECT * FROM sp;
        """,
        out=Csv("""
        "ts","dur","part"
        500,100,10
        """))

  def test_span_outer_join_unpartitioned_empty(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query="""
        CREATE TABLE t1(
          ts BIGINT,
          dur BIGINT,
          PRIMARY KEY (ts)
        ) WITHOUT ROWID;

        CREATE TABLE t2(
          ts BIGINT,
          dur BIGINT,
          PRIMARY KEY (ts)
        ) WITHOUT ROWID;


        CREATE VIRTUAL TABLE sp USING span_outer_join(t1, t2);

        SELECT * FROM sp;
        """,
        out=Csv("""
        "ts","dur"
        """))

  def test_span_outer_join_unpartitioned_left_empty(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query="""
        CREATE TABLE t1(
          ts BIGINT,
          dur BIGINT,
          PRIMARY KEY (ts)
        ) WITHOUT ROWID;

        CREATE TABLE t2(
          ts BIGINT,
          dur BIGINT,
          PRIMARY KEY (ts)
        ) WITHOUT ROWID;

        INSERT INTO t2(ts, dur)
        VALUES
        (100, 400),
        (500, 50),
        (600, 100);

        CREATE VIRTUAL TABLE sp USING span_outer_join(t1, t2);

        SELECT * FROM sp;
        """,
        out=Csv("""
        "ts","dur"
        100,400
        500,50
        600,100
        """))

  def test_span_outer_join_unpartitioned_right_empty(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query="""
        CREATE TABLE t1(
          ts BIGINT,
          dur BIGINT,
          PRIMARY KEY (ts)
        ) WITHOUT ROWID;

        CREATE TABLE t2(
          ts BIGINT,
          dur BIGINT,
          PRIMARY KEY (ts)
        ) WITHOUT ROWID;

        INSERT INTO t1(ts, dur)
        VALUES
        (100, 400),
        (500, 50),
        (600, 100);

        CREATE VIRTUAL TABLE sp USING span_outer_join(t1, t2);

        SELECT * FROM sp;
        """,
        out=Csv("""
        "ts","dur"
        100,400
        500,50
        600,100
        """))

  def test_span_outer_join_mixed(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query=Path('span_outer_join_mixed_test.sql'),
        out=Path('span_outer_join_mixed.out'))

  def test_span_outer_join_mixed_empty(self):
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
          PRIMARY KEY (ts)
        ) WITHOUT ROWID;


        CREATE VIRTUAL TABLE sp USING span_outer_join(t1 PARTITIONED part, t2);

        SELECT * FROM sp;
        """,
        out=Csv("""
        "ts","dur","part"
        """))

  def test_span_outer_join_mixed_left_empty(self):
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
          PRIMARY KEY (ts)
        ) WITHOUT ROWID;

        INSERT INTO t2(ts, dur)
        VALUES
        (100, 400),
        (500, 50),
        (600, 100);

        CREATE VIRTUAL TABLE sp USING span_outer_join(t1 PARTITIONED part, t2);

        SELECT * FROM sp;
        """,
        out=Csv("""
        "ts","dur","part"
        """))

  def test_span_outer_join_mixed_left_empty_rev(self):
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
          PRIMARY KEY (ts)
        ) WITHOUT ROWID;

        INSERT INTO t1(ts, dur, part)
        VALUES
        (100, 400, 0),
        (100, 50, 1),
        (600, 100, 1);

        CREATE VIRTUAL TABLE sp USING span_outer_join(t2, t1 PARTITIONED part);

        SELECT * FROM sp;
        """,
        out=Csv("""
        "ts","dur","part"
        100,400,0
        100,50,1
        600,100,1
        """))

  def test_span_outer_join_mixed_right_empty(self):
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
          b BIGINT,
          PRIMARY KEY (ts)
        ) WITHOUT ROWID;

        INSERT INTO t1(ts, dur, part)
        VALUES
        (100, 400, 0),
        (100, 50, 1),
        (600, 100, 1);

        CREATE VIRTUAL TABLE sp USING span_outer_join(t1 PARTITIONED part, t2);

        SELECT * FROM sp;
        """,
        out=Csv("""
        "ts","dur","part","b"
        100,400,0,"[NULL]"
        100,50,1,"[NULL]"
        600,100,1,"[NULL]"
        """))

  def test_span_outer_join_mixed_right_empty_rev(self):
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
          b BIGINT,
          PRIMARY KEY (ts)
        ) WITHOUT ROWID;

        INSERT INTO t2(ts, dur)
        VALUES
        (100, 400),
        (500, 50),
        (600, 100);

        CREATE VIRTUAL TABLE sp USING span_outer_join(t2, t1 PARTITIONED part);

        SELECT * FROM sp;
        """,
        out=Csv("""
        "ts","dur","part","b"
        """))

  def test_span_outer_join_mixed_2(self):
    return DiffTestBlueprint(
        trace=Path('../../common/synth_1.py'),
        query=Path('span_outer_join_mixed_test.sql'),
        out=Path('span_outer_join_mixed.out'))
