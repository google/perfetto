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


class SpanJoinRegression(TestSuite):

  def test_android_sched_and_ps_slice_span_join_b118665515(self):
    return DiffTestBlueprint(
        trace=DataPath('android_sched_and_ps.pb'),
        query=Path('slice_span_join_b118665515_test.sql'),
        out=Path('android_sched_and_ps_slice_span_join_b118665515.out'))

  def test_span_join_unpartitioned_empty(self):
    return DiffTestBlueprint(
        trace=DataPath('android_sched_and_ps.pb'),
        query="""
        CREATE TABLE t1(
          ts BIGINT,
          dur BIGINT,
          PRIMARY KEY (ts, dur)
        ) WITHOUT ROWID;

        CREATE TABLE t2(
          ts BIGINT,
          dur BIGINT,
          PRIMARY KEY (ts, dur)
        ) WITHOUT ROWID;

        INSERT INTO t2(ts, dur)
        VALUES
        (1, 2),
        (5, 0),
        (1, 1);

        CREATE VIRTUAL TABLE sp USING span_join(t1, t2);

        SELECT ts, dur FROM sp;
        """,
        out=Csv("""
        "ts","dur"
        """))
