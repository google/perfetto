#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class GraphPartitionTests(TestSuite):

  def test_tree_structural_partition(self):
    return DiffTestBlueprint(
        trace=DataPath('counters.json'),
        query="""
          INCLUDE PERFETTO MODULE graphs.partition;

          -- See the comment in the graphs.partition module for a visual
          -- representation of this graph.
          CREATE PERFETTO TABLE foo AS
          WITH data(id, parent_id, group_key) AS (
            VALUES
            (1, NULL, 1),
            (2, 1,    1),
            (3, 2,    2),
            (4, 2,    2),
            (5, 4,    1),
            (6, 4,    3),
            (7, 4,    2),
            (8, 4,    1)
          )
          SELECT * FROM data;

          SELECT *
          FROM tree_structural_partition_by_group!(foo)
          ORDER BY id;
        """,
        out=Csv("""
        "id","parent_id","group_key"
        1,"[NULL]",1
        2,1,1
        3,"[NULL]",2
        4,"[NULL]",2
        5,2,1
        6,"[NULL]",3
        7,4,2
        8,2,1
        """))
