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


class PreludeWindowFunctions(TestSuite):

  def test_first_non_null_frame(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        CREATE TABLE TEST(id INTEGER, val INTEGER);

        INSERT INTO TEST
        VALUES (1, 1), (2, NULL), (3, 3), (4, 4), (5, NULL), (6, NULL), (7, NULL);

        SELECT
          id,
          LAST_NON_NULL(val)
          OVER (ORDER BY id ASC ROWS BETWEEN CURRENT ROW AND 2 FOLLOWING) AS val
        FROM TEST
        ORDER BY id ASC;
        """,
        out=Csv("""
        "id","val"
        1,3
        2,4
        3,4
        4,4
        5,"[NULL]"
        6,"[NULL]"
        7,"[NULL]"
        """))

  def test_first_non_null_partition(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        CREATE TABLE TEST(id INTEGER, part TEXT, val INTEGER);

        INSERT INTO TEST
        VALUES
        (1, 'A', 1),
        (2, 'A', NULL),
        (3, 'A', 3),
        (4, 'B', NULL),
        (5, 'B', 5),
        (6, 'B', NULL),
        (7, 'B', 7);

        SELECT id, LAST_NON_NULL(val) OVER (PARTITION BY part ORDER BY id ASC) AS val
        FROM TEST
        ORDER BY id ASC;
        """,
        out=Csv("""
        "id","val"
        1,1
        2,1
        3,3
        4,"[NULL]"
        5,5
        6,5
        7,7
        """))

  def test_first_non_null(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        CREATE TABLE TEST(id INTEGER, val INTEGER);

        INSERT INTO TEST
        VALUES (1, 1), (2, NULL), (3, 3), (4, 4), (5, NULL), (6, NULL), (7, NULL);

        SELECT id, LAST_NON_NULL(val) OVER (ORDER BY id ASC) AS val
        FROM TEST
        ORDER BY id ASC;
        """,
        out=Csv("""
        "id","val"
        1,1
        2,1
        3,3
        4,4
        5,4
        6,4
        7,4
        """))

  def test_layout(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        """),
        query="""
        CREATE TABLE TEST(start INTEGER, end INTEGER);

        INSERT INTO TEST
        VALUES
        (1, 5),
        (2, 4),
        (3, 8),
        (6, 7),
        (6, 7),
        (6, 7);

        WITH custom_slices as (
          SELECT
            start as ts,
            end - start as dur
          FROM test
        )
        SELECT
          ts,
          INTERNAL_LAYOUT(ts, dur) over (
            order by ts
            rows between unbounded preceding and current row
          ) as depth
        FROM custom_slices
        """,
        out=Csv("""
        "ts","depth"
        1,0
        2,1
        3,2
        6,0
        6,1
        6,3
        """))

  def test_layout_with_instant_events(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        """),
        query="""
        CREATE TABLE TEST(start INTEGER, end INTEGER);

        INSERT INTO TEST
        VALUES
        (1, 5),
        (2, 2),
        (3, 3),
        (4, 4);

        WITH custom_slices as (
          SELECT
            start as ts,
            end - start as dur
          FROM test
        )
        SELECT
          ts,
          INTERNAL_LAYOUT(ts, dur) over (
            order by ts
            rows between unbounded preceding and current row
          ) as depth
        FROM custom_slices
        """,
        out=Csv("""
        "ts","depth"
        1,0
        2,1
        3,1
        4,1
        """))

  def test_layout_with_events_without_end(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        """),
        query="""
        CREATE TABLE TEST(ts INTEGER, dur INTEGER);

        INSERT INTO TEST
        VALUES
        (1, -1),
        (2, -1),
        (3, 5),
        (4, 1),
        (5, 1);

        SELECT
          ts,
          INTERNAL_LAYOUT(ts, dur) over (
            order by ts
            rows between unbounded preceding and current row
          ) as depth
        FROM test
        """,
        out=Csv("""
        "ts","depth"
        1,0
        2,1
        3,2
        4,3
        5,3
        """))
