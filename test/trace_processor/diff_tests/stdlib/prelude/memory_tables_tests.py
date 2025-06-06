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


class PreludeMemoryTables(TestSuite):
  # Currently empty due to facing an issue in populating any memory snapshot tables
  def test_memory_snapshot(self):
    return DiffTestBlueprint(
        trace=Path('../../../../../test/data/example_android_trace_30s.pb'),
        query="""
            SELECT *
            FROM memory_snapshot
            """,
        out=Csv("""
            "id","ts","track_id","detail_level"
            """))

  def test_process_memory_snapshot(self):
    return DiffTestBlueprint(
        trace=Path('../../../../../test/data/example_android_trace_30s.pb'),
        query="""
            SELECT *
            FROM process_memory_snapshot
            """,
        out=Csv("""
            "id","snapshot_id","upid"
            """))

  def test_memory_snapshot_node(self):
    return DiffTestBlueprint(
        trace=Path('../../../../../test/data/example_android_trace_30s.pb'),
        query="""
            SELECT *
            FROM memory_snapshot_node
            """,
        out=Csv("""
            "id","process_snapshot_id","parent_node_id","path","size","effective_size","arg_set_id"
            """))

  def test_memory_snapshot_edge(self):
    return DiffTestBlueprint(
        trace=Path('../../../../../test/data/example_android_trace_30s.pb'),
        query="""
            SELECT *
            FROM memory_snapshot_edge
            """,
        out=Csv("""
            "id","source_node_id","target_node_id","importance"
            """))
