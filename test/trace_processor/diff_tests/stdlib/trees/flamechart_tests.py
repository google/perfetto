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

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class FlamechartRuns(TestSuite):

  def test_shared_prefix_merges(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.flamechart;

          CREATE PERFETTO TABLE stacks AS
          SELECT 100 AS id, NULL AS parent_id, 'A' AS name
          UNION ALL SELECT 101, 100, 'B'
          UNION ALL SELECT 102, 101, 'C'
          UNION ALL SELECT 103, 101, 'D';

          CREATE PERFETTO TABLE points AS
          SELECT 10 AS ts, 102 AS leaf_id
          UNION ALL SELECT 20, 102
          UNION ALL SELECT 30, 103
          UNION ALL SELECT 40, 103;

          SELECT ts, dur, depth, id, sample_count
          FROM _flamechart_runs!(
            _tree_from_table!((SELECT * FROM stacks), (name)),
            (SELECT ts, leaf_id FROM points ORDER BY ts)
          )
          ORDER BY depth, ts;
        """,
        out=Csv("""
        "ts","dur","depth","id","sample_count"
        10,30,0,100,4
        10,30,1,101,4
        10,20,2,102,2
        30,10,2,103,2
        """))

  def test_null_and_unresolvable_leaves_skipped(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.flamechart;

          CREATE PERFETTO TABLE stacks AS
          SELECT 100 AS id, NULL AS parent_id, 'A' AS name
          UNION ALL SELECT 101, 100, 'B';

          CREATE PERFETTO TABLE points AS
          SELECT 10 AS ts, 101 AS leaf_id
          UNION ALL SELECT 15, NULL
          UNION ALL SELECT 18, 999
          UNION ALL SELECT 20, 101;

          SELECT ts, dur, depth, id, sample_count
          FROM _flamechart_runs!(
            _tree_from_table!((SELECT * FROM stacks), (name)),
            (SELECT ts, leaf_id FROM points ORDER BY ts)
          )
          ORDER BY depth, ts;
        """,
        out=Csv("""
        "ts","dur","depth","id","sample_count"
        10,10,0,100,2
        10,10,1,101,2
        """))

  def test_empty_points(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE std.trees.table_conversion;
          INCLUDE PERFETTO MODULE std.trees.flamechart;

          CREATE PERFETTO TABLE stacks AS
          SELECT 100 AS id, NULL AS parent_id, 'A' AS name;

          SELECT count(*) AS cnt
          FROM _flamechart_runs!(
            _tree_from_table!((SELECT * FROM stacks), (name)),
            (SELECT 0 AS ts, 0 AS leaf_id WHERE FALSE)
          );
        """,
        out=Csv("""
        "cnt"
        0
        """))
