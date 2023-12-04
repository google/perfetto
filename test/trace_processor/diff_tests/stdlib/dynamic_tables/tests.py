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


class DynamicTables(TestSuite):
  # Tests for custom dynamic tables. Ancestor slice table.
  def test_ancestor_slice(self):
    return DiffTestBlueprint(
        trace=Path('relationship_tables.textproto'),
        query="""
        SELECT slice.name AS currentSliceName, ancestor.name AS ancestorSliceName
        FROM slice LEFT JOIN ancestor_slice(slice.id) AS ancestor
        ORDER BY slice.ts ASC, ancestor.ts ASC, slice.name ASC, ancestor.name ASC;
        """,
        out=Path('ancestor_slice.out'))

  # Descendant slice table.
  def test_descendant_slice(self):
    return DiffTestBlueprint(
        trace=Path('relationship_tables.textproto'),
        query="""
        SELECT slice.name AS currentSliceName, descendant.name AS descendantSliceName
        FROM slice LEFT JOIN descendant_slice(slice.id) AS descendant
        ORDER BY slice.ts ASC, descendant.ts ASC, slice.name ASC, descendant.name ASC;
        """,
        out=Path('descendant_slice.out'))

  # Ancestor slice by stack table.
  def test_ancestor_slice_by_stack(self):
    return DiffTestBlueprint(
        trace=Path('slice_stacks.textproto'),
        query="""
        SELECT ts, name FROM ancestor_slice_by_stack((
          SELECT stack_id FROM slice
          WHERE name = 'event_depth_2'
          LIMIT 1
          ));
        """,
        out=Csv("""
        "ts","name"
        1000,"event_depth_0"
        2000,"event_depth_1"
        8000,"event_depth_0"
        9000,"event_depth_1"
        """))

  # Descendant slice by stack table.
  def test_descendant_slice_by_stack(self):
    return DiffTestBlueprint(
        trace=Path('slice_stacks.textproto'),
        query="""
        SELECT ts, name FROM descendant_slice_by_stack((
          SELECT stack_id FROM slice
          WHERE name = 'event_depth_0'
          LIMIT 1
          ));
        """,
        out=Csv("""
        "ts","name"
        2000,"event_depth_1"
        3000,"event_depth_2"
        9000,"event_depth_1"
        10000,"event_depth_2"
        """))

  # Connected/Following/Perceeding flow table.
  def test_connected_flow(self):
    return DiffTestBlueprint(
        trace=Path('connected_flow_data.json'),
        query=Path('connected_flow_test.sql'),
        out=Path('connected_flow.out'))

  # Annotated callstacks.
  def test_perf_sample_sc_annotated_callstack(self):
    return DiffTestBlueprint(
        trace=DataPath('perf_sample_sc.pb'),
        query="""
        SELECT eac.id, eac.depth, eac.frame_id, eac.annotation,
               spf.name
        FROM experimental_annotated_callstack eac
        JOIN perf_sample ps
          ON (eac.start_id = ps.callsite_id)
        JOIN stack_profile_frame spf
          ON (eac.frame_id = spf.id)
        ORDER BY eac.start_id ASC, eac.depth ASC;
        """,
        out=Path('perf_sample_sc_annotated_callstack.out'))

  # ABS_TIME_STR function
  def test_various_clocks_abs_time_str(self):
    return DiffTestBlueprint(
        trace=Path('various_clocks.textproto'),
        query="""
        SELECT
          ABS_TIME_STR(15) AS t15,
          ABS_TIME_STR(25) AS t25,
          ABS_TIME_STR(35) AS t35;
        """,
        out=Csv("""
        "t15","t25","t35"
        "1970-01-01T00:00:00.000000005","2022-05-18T19:59:59.999999995","2022-05-18T20:00:00.000000000"
        """))

  def test_empty_abs_time_str(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        SELECT
          ABS_TIME_STR(15) AS t15,
          ABS_TIME_STR(25) AS t25,
          ABS_TIME_STR(35) AS t35;
        """,
        out=Csv("""
        "t15","t25","t35"
        "[NULL]","[NULL]","[NULL]"
        """))

  # TO_REALTIME function
  def test_various_clocks_to_realtime(self):
    return DiffTestBlueprint(
        trace=Path('various_clocks.textproto'),
        query="""
        SELECT
          TO_REALTIME(15) AS t15,
          TO_REALTIME(25) AS t25,
          TO_REALTIME(35) AS t35;
        """,
        out=Csv("""
        "t15","t25","t35"
        5,1652903999999999995,1652904000000000000
        """))

  def test_empty_to_realtime(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        SELECT
          TO_REALTIME(15) AS t15,
          TO_REALTIME(25) AS t25,
          TO_REALTIME(35) AS t35;
        """,
        out=Csv("""
        "t15","t25","t35"
        "[NULL]","[NULL]","[NULL]"
        """))

  # TO_MONOTONIC function
  def test_various_clocks_to_monotonic(self):
    return DiffTestBlueprint(
        trace=Path('various_clocks.textproto'),
        query="""
        SELECT
          TO_MONOTONIC(25) AS t15,
          TO_MONOTONIC(35) AS t20,
          TO_MONOTONIC(50) AS t25;
        """,
        out=Csv("""
        "t15","t20","t25"
        15,20,25
        """))

  def test_empty_to_monotonic(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""

        """),
        query="""
        SELECT
          TO_MONOTONIC(25) AS t15,
          TO_MONOTONIC(35) AS t20,
          TO_MONOTONIC(50) AS t25;
        """,
        out=Csv("""
        "t15","t20","t25"
        "[NULL]","[NULL]","[NULL]"
        """))

  # TO_TIMECODE function
  def test_various_clocks_to_timecode(self):
    return DiffTestBlueprint(
        trace=Path('various_clocks.textproto'),
        query="""
        SELECT
          TO_TIMECODE(0) AS t0,
          TO_TIMECODE(123456789123456789) AS tN
        """,
        out=Csv("""
        "t0","tN"
        "00:00:00 000 000 000","33:33:09 123 456 789"
        """))
