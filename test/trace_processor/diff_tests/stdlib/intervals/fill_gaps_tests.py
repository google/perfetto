#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, TextProto, DiffTestBlueprint, TestSuite


def make_trace_bounds(start: int, end: int) -> TextProto:
  return TextProto(f"""
  packet {{
    ftrace_events {{
      cpu: 0
      event {{
        timestamp: {start}
        pid: 10
        print {{
          buf: "B|10|Start"
        }}
      }}
      event {{
        timestamp: {end}
        pid: 10
        print {{
          buf: "E|10"
        }}
      }}
    }}
  }}
  """)


class IntervalsFillGaps(TestSuite):

  def test_fill_gaps_single_group(self):
    return DiffTestBlueprint(
        trace=make_trace_bounds(100, 1000),
        query="""
        INCLUDE PERFETTO MODULE intervals.fill_gaps;

        CREATE PERFETTO TABLE data_table AS
        WITH data(ts, dur, grp, val) AS (
          VALUES
            (200, 100, 'A', 42),
            (500, 200, 'A', 43)
        )
        SELECT * FROM data;

        SELECT ts, dur, grp, val
        FROM _intervals_fill_gaps!((grp), (val), data_table)
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","grp","val"
        100,100,"A","[NULL]"
        200,100,"A",42
        300,200,"A","[NULL]"
        500,200,"A",43
        700,300,"A","[NULL]"
        """))

  def test_fill_gaps_multiple_groups(self):
    return DiffTestBlueprint(
        trace=make_trace_bounds(100, 1000),
        query="""
        INCLUDE PERFETTO MODULE intervals.fill_gaps;

        CREATE PERFETTO TABLE data_table AS
        WITH data(ts, dur, grp, val) AS (
          VALUES
            (200, 100, 'A', 42),
            (500, 200, 'A', 43),
            (300, 150, 'B', 99)
        )
        SELECT * FROM data;

        SELECT ts, dur, grp, val
        FROM _intervals_fill_gaps!((grp), (val), data_table)
        ORDER BY grp, ts;
        """,
        out=Csv("""
        "ts","dur","grp","val"
        100,100,"A","[NULL]"
        200,100,"A",42
        300,200,"A","[NULL]"
        500,200,"A",43
        700,300,"A","[NULL]"
        100,200,"B","[NULL]"
        300,150,"B",99
        450,550,"B","[NULL]"
        """))

  def test_fill_gaps_empty_input(self):
    return DiffTestBlueprint(
        trace=make_trace_bounds(100, 1000),
        query="""
        INCLUDE PERFETTO MODULE intervals.fill_gaps;

        CREATE PERFETTO TABLE empty_table AS
        SELECT CAST(NULL AS INT) AS ts, CAST(NULL AS INT) AS dur, CAST(NULL AS TEXT) AS grp, CAST(NULL AS INT) AS val LIMIT 0;

        SELECT ts, dur, grp, val
        FROM _intervals_fill_gaps!((grp), (val), empty_table);
        """,
        out=Csv("""
        "ts","dur","grp","val"
        """))

  def test_fill_gaps_no_valid_slices(self):
    return DiffTestBlueprint(
        trace=make_trace_bounds(100, 1000),
        query="""
        INCLUDE PERFETTO MODULE intervals.fill_gaps;

        CREATE PERFETTO TABLE data_table AS
        WITH data(ts, dur, grp, val) AS (
          VALUES
            (NULL, NULL, 'A', NULL)
        )
        SELECT * FROM data;

        SELECT ts, dur, grp, val
        FROM _intervals_fill_gaps!((grp), (val), data_table);
        """,
        out=Csv("""
        "ts","dur","grp","val"
        100,900,"A","[NULL]"
        """))

  def test_fill_gaps_multiple_group_cols(self):
    return DiffTestBlueprint(
        trace=make_trace_bounds(100, 1000),
        query="""
        INCLUDE PERFETTO MODULE intervals.fill_gaps;

        CREATE PERFETTO TABLE data_table AS
        WITH data(ts, dur, grp1, grp2, val) AS (
          VALUES
            (200, 100, 'A', 'X', 42),
            (500, 200, 'A', 'Y', 43)
        )
        SELECT * FROM data;

        SELECT ts, dur, grp1, grp2, val
        FROM _intervals_fill_gaps!((grp1, grp2), (val), data_table)
        ORDER BY grp1, grp2, ts;
        """,
        out=Csv("""
        "ts","dur","grp1","grp2","val"
        100,100,"A","X","[NULL]"
        200,100,"A","X",42
        300,700,"A","X","[NULL]"
        100,400,"A","Y","[NULL]"
        500,200,"A","Y",43
        700,300,"A","Y","[NULL]"
        """))

  def test_fill_gaps_multiple_data_cols(self):
    return DiffTestBlueprint(
        trace=make_trace_bounds(100, 1000),
        query="""
        INCLUDE PERFETTO MODULE intervals.fill_gaps;

        CREATE PERFETTO TABLE data_table AS
        WITH data(ts, dur, grp, val1, val2) AS (
          VALUES
            (200, 100, 'A', 42, 'hello'),
            (500, 200, 'A', 43, 'world')
        )
        SELECT * FROM data;

        SELECT ts, dur, grp, val1, val2
        FROM _intervals_fill_gaps!((grp), (val1, val2), data_table)
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","grp","val1","val2"
        100,100,"A","[NULL]","[NULL]"
        200,100,"A",42,"hello"
        300,200,"A","[NULL]","[NULL]"
        500,200,"A",43,"world"
        700,300,"A","[NULL]","[NULL]"
        """))

  def test_fill_gaps_union_guarantees_group(self):
    return DiffTestBlueprint(
        trace=make_trace_bounds(100, 1000),
        query="""
        INCLUDE PERFETTO MODULE intervals.fill_gaps;

        CREATE PERFETTO TABLE data_table AS
        WITH data(ts, dur, uid, val) AS (
          VALUES
            (200, 100, 1234, 42),
            (NULL, NULL, 1234, NULL)
        )
        SELECT * FROM data;

        SELECT ts, dur, uid, val
        FROM _intervals_fill_gaps!((uid), (val), data_table)
        ORDER BY uid, ts;
        """,
        out=Csv("""
        "ts","dur","uid","val"
        100,100,1234,"[NULL]"
        200,100,1234,42
        300,700,1234,"[NULL]"
        """))

  def test_fill_gaps_without_partition(self):
    return DiffTestBlueprint(
        trace=make_trace_bounds(100, 1000),
        query="""
        INCLUDE PERFETTO MODULE intervals.fill_gaps;

        CREATE PERFETTO TABLE data_table AS
        WITH data(ts, dur, val) AS (
          VALUES
            (200, 100, 42)
        )
        SELECT * FROM data;

        SELECT ts, dur, val
        FROM _intervals_fill_gaps!((NULL), (val), data_table)
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","val"
        100,100,"[NULL]"
        200,100,42
        300,700,"[NULL]"
        """))