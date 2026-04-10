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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


# Tests a simple ProtoVM program that consumes surfaceflinger_transaction packets
# as patches and applies them to surfaceflinger_layers_snapshot packets.
# See protovm_incremental_tracing.textproto for more details.
class ProtoVmIncrementalTracing(TestSuite):

  def test_timestamps(self):
    return DiffTestBlueprint(
        trace=Path('protovm_incremental_tracing.textproto'),
        query="""
        SELECT
          id, ts
        FROM
          surfaceflinger_layers_snapshot LIMIT 2;
        """,
        out=Csv("""
        "id","ts"
        0,2749532000001
        1,2749555000001
        """))

  def test_vsync_id(self):
    return DiffTestBlueprint(
        trace=Path('protovm_incremental_tracing.textproto'),
        query="""
        SELECT
          args.display_value
        FROM
          surfaceflinger_layers_snapshot AS sfs JOIN args ON sfs.arg_set_id = args.arg_set_id
        WHERE args.key = "vsync_id"
        ORDER BY sfs.id;
        """,
        out=Csv("""
        "display_value"
        "24776"
        "24805"
        """))

  def test_where(self):
    return DiffTestBlueprint(
        trace=Path('protovm_incremental_tracing.textproto'),
        query="""
        SELECT
          args.display_value
        FROM
          surfaceflinger_layers_snapshot AS sfs JOIN args ON sfs.arg_set_id = args.arg_set_id
        WHERE args.key = "where"
        ORDER BY sfs.id;
        """,
        out=Csv("""
        "display_value"
        "value set in initial state and never touched by the ProtoVM"
        "value set in initial state and never touched by the ProtoVM"
        """))
