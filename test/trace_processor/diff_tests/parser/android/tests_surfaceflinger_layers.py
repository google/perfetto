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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class SurfaceFlingerLayers(TestSuite):

  def test_snapshot_table(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        SELECT
          id, ts
        FROM
          surfaceflinger_layers_snapshot;
        """,
        out=Csv("""
        "id","ts"
        0,2748300281655
        1,2749500341063
        """))

  def test_snapshot_args(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        SELECT
          args.key, args.display_value
        FROM
          surfaceflinger_layers_snapshot AS sfs JOIN args ON sfs.arg_set_id = args.arg_set_id
        WHERE sfs.id = 0 and args.key != "hwc_blob"
        ORDER BY args.key;
        """,
        out=Csv("""
        "key","display_value"
        "displays[0].id","4619827677550801152"
        "displays[0].is_virtual","false"
        "displays[0].layer_stack","0"
        "displays[0].layer_stack_space_rect.bottom","2400"
        "displays[0].layer_stack_space_rect.left","0"
        "displays[0].layer_stack_space_rect.right","1080"
        "displays[0].layer_stack_space_rect.top","0"
        "displays[0].name","Common Panel"
        "displays[0].size.h","2400"
        "displays[0].size.w","1080"
        "displays[0].transform.type","0"
        "elapsed_realtime_nanos","2748300281655"
        "vsync_id","24766"
        "where","visibleRegionsDirty"
        """))

  def test_layer_table(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        SELECT
          id, snapshot_id, type
        FROM
          surfaceflinger_layer;
        """,
        out=Csv("""
        "id","snapshot_id","type"
        0,0,"surfaceflinger_layer"
        1,0,"surfaceflinger_layer"
        2,1,"surfaceflinger_layer"
        3,1,"surfaceflinger_layer"
        """))

  def test_tables_have_raw_protos(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        SELECT COUNT(*) FROM surfaceflinger_layers_snapshot
        WHERE base64_proto IS NOT NULL AND base64_proto_id IS NOT NULL
        UNION ALL
        SELECT COUNT(*) FROM surfaceflinger_layer
        WHERE base64_proto IS NOT NULL AND base64_proto_id IS NOT NULL
        """,
        out=Csv("""
        "COUNT(*)"
        2
        4
        """))
