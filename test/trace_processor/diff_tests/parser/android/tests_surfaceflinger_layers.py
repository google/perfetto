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
          surfaceflinger_layers_snapshot LIMIT 2;
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

  def test_displays_table(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        SELECT * FROM android_surfaceflinger_display LIMIT 5;
        """,
        out=Csv("""
        "id","snapshot_id","is_on","is_virtual","trace_rect_id","display_id","display_name"
        0,0,1,0,0,4619827677550801152,"Common Panel"
        1,1,1,1,1,4619827677550801152,"Common Panel"
        2,1,0,0,2,4619827677550801153,"Common Panel"
        3,2,1,0,4,4619827677550801152,"[NULL]"
        4,2,0,0,5,4619827677550801153,"Other"
        """))

  def test_layer_table(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        SELECT
          id, snapshot_id, layer_id, layer_name, parent, corner_radius, hwc_composition_type, z_order_relative_of, is_missing_z_parent
        FROM
          surfaceflinger_layer
        LIMIT 5;
        """,
        out=Csv("""
        "id","snapshot_id","layer_id","layer_name","parent","corner_radius","hwc_composition_type","z_order_relative_of","is_missing_z_parent"
        0,0,3,"Display 0 name="Built-in Screen"#3","[NULL]",0.000000,"[NULL]",5,1
        1,0,4,"WindowedMagnification:0:31#4",3,0.000000,"[NULL]","[NULL]",0
        2,1,3,"Display 0 name="Built-in Screen"#3","[NULL]",0.000000,"[NULL]","[NULL]",0
        3,1,4,"WindowedMagnification:0:31#4",3,0.000000,"[NULL]","[NULL]",0
        4,2,1,"layer1","[NULL]",1.000000,2,"[NULL]",0
        """))

  def test_tables_have_raw_protos(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        SELECT COUNT(*) FROM surfaceflinger_layers_snapshot
        WHERE base64_proto_id IS NOT NULL
        UNION ALL
        SELECT COUNT(*) FROM surfaceflinger_layer
        WHERE base64_proto_id IS NOT NULL
        """,
        out=Csv("""
        "COUNT(*)"
        4
        29
        """))

  def test_layer_args(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        SELECT
          args.key, args.display_value
        FROM
          surfaceflinger_layer AS sfl JOIN args ON sfl.arg_set_id = args.arg_set_id
        WHERE sfl.id = 2 and (key like "screen_bounds%" or key like "visibility_reason%")
        ORDER BY args.key
        """,
        out=Csv("""
        "key","display_value"
        "screen_bounds.bottom","24000.0"
        "screen_bounds.left","-10800.0"
        "screen_bounds.right","10800.0"
        "screen_bounds.top","-24000.0"
        "visibility_reason[0]","buffer is empty"
        "visibility_reason[1]","does not have color fill, shadow or blur"
        """))

  def test_layer_extraction(self):
    # sorts by z
    # handles relative layers
    # handles negative z values
    # falls back to layer id comparison for equal z values
    # only falls back to layer id for siblings
    # restricts z-order sorting to each level of hierarchy
    # sets depth according to z order
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        INCLUDE PERFETTO MODULE android.winscope.rect;
        SELECT
          layer_id, depth
        FROM surfaceflinger_layer AS sfl
        INNER JOIN android_winscope_trace_rect as rect
        ON sfl.snapshot_id = 2 and rect.id = sfl.layer_rect_id
        ORDER BY depth;
        """,
        out=Csv("""
        "layer_id","depth"
        6,1
        11,2
        3,3
        5,4
        7,5
        1,6
        4,7
        2,8
        9,9
        8,10
        10,11
        """))

  def test_layer_visible_in_isolation(self):
    # 4 (snapshot 1) not visible: empty bounds
    # 6 visible: non-empty visible region
    # 11 not visible: hidden by parent even though relz parent not hidden
    # 3 not visible: layer hidden by policy
    # 5 not visible: zero alpha
    # 7 not visible: null active buffer and no effects
    # 1 not visible: empty active buffer and no effects
    # 4 (snapshot 2) not visible: null visible region
    # 2 not visible: empty visible region
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        INCLUDE PERFETTO MODULE android.winscope.rect;

        SELECT
          layer_id, sfl.is_visible, rect.is_visible as rect_is_visible
        FROM surfaceflinger_layer AS sfl
        INNER JOIN android_winscope_trace_rect as rect
        ON ((snapshot_id = 1 and layer_id = 4) OR snapshot_id = 2)
          AND rect.id = sfl.layer_rect_id
        ORDER BY snapshot_id, depth
        LIMIT 9
        """,
        out=Csv("""
        "layer_id","is_visible","rect_is_visible"
        4,0,0
        6,1,1
        11,0,0
        3,0,0
        5,0,0
        7,0,0
        1,0,0
        4,0,0
        2,0,0
        """))

  def test_layer_occlusion(self):
    # 1 occluded by 2
    # 3 translucent due to 0.5 alpha - covers 1 and 2
    # 4 not occluded by 2 due to different layer stack
    # 5 partially occludes 2 and 3
    # 7 translucent due to is_opaque flag not set - covers 6
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        INCLUDE PERFETTO MODULE android.winscope.rect;

        SELECT
          sfl.layer_id,
          sfl.is_visible,
          visibility.display_value AS visibility_reason,
          occluded_by.display_value AS occluded_by,
          partially_occluded_by.display_value AS partially_occluded_by,
          covered_by.display_value AS covered_by
        FROM surfaceflinger_layer AS sfl

        LEFT JOIN args as visibility
          ON sfl.arg_set_id = visibility.arg_set_id
          AND visibility.flat_key = 'visibility_reason'

        LEFT JOIN args as occluded_by
          ON sfl.arg_set_id = occluded_by.arg_set_id
          AND occluded_by.flat_key = 'occluded_by'

        LEFT JOIN args as partially_occluded_by
          ON sfl.arg_set_id = partially_occluded_by.arg_set_id
          AND partially_occluded_by.flat_key = 'partially_occluded_by'

        LEFT JOIN args as covered_by
          ON sfl.arg_set_id = covered_by.arg_set_id
          AND covered_by.flat_key = 'covered_by'

        WHERE sfl.snapshot_id = 3

        LIMIT 9
        """,
        out=Csv("""
        "layer_id","is_visible","visibility_reason","occluded_by","partially_occluded_by","covered_by"
        1,0,"occluded","2","[NULL]","3"
        1,0,"occluded","5","[NULL]","3"
        2,1,"[NULL]","[NULL]","5","3"
        3,1,"[NULL]","[NULL]","5","[NULL]"
        4,1,"[NULL]","[NULL]","[NULL]","[NULL]"
        5,1,"[NULL]","[NULL]","[NULL]","[NULL]"
        6,1,"[NULL]","[NULL]","[NULL]","7"
        7,1,"[NULL]","[NULL]","[NULL]","[NULL]"
        8,1,"[NULL]","[NULL]","[NULL]","[NULL]"
        """))

  def test_layer_rects(self):
    # gives group_id 1 for 4 and resets depth due to different layer stack
    # ignores 8 as missing screen bounds
    # makes 9 as invalid bounds but visible rect
    # ignores 10 as invalid screen bounds from display 3
    # ignores 11 as invalid screen bounds from display 3 (float not exactly equal)
    # ignores 12 as invalid screen bounds from platform default
    # ignores 13 as invalid screen bounds from display 4 (rotated)
    # ignores 14 as invalid screen bounds from max of all available displays
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        INCLUDE PERFETTO MODULE android.winscope.rect;

        SELECT
          sfl.layer_id, wtr.group_id, wtr.depth, wtr.is_visible, wtr.opacity, rect.x, rect.y, rect.w, rect.h
        FROM surfaceflinger_layer AS sfl
        INNER JOIN android_winscope_trace_rect AS wtr
          ON sfl.layer_rect_id = wtr.id
          AND sfl.snapshot_id = 3
        INNER JOIN android_winscope_rect AS rect
          ON rect.id = wtr.rect_id
        """,
        out=Csv("""
        "layer_id","group_id","depth","is_visible","opacity","x","y","w","h"
        1,0,1,0,1.000000,0.000000,0.000000,1.000000,1.000000
        2,0,2,1,1.000000,0.000000,0.000000,2.000000,2.000000
        3,0,3,1,0.500000,0.000000,0.000000,2.000000,2.000000
        4,1,1,1,1.000000,0.000000,0.000000,1.000000,1.000000
        5,0,4,1,1.000000,0.000000,0.000000,1.000000,1.000000
        6,0,5,1,1.000000,2.000000,2.000000,1.000000,1.000000
        7,0,6,1,1.000000,2.000000,2.000000,1.000000,1.000000
        9,2,1,1,1.000000,-50.000000,-100.000000,100.000000,200.000000
        """))

  def test_display_rects(self):
    # makes display rects from stack space rect if available
    # sets group id from layer stack
    # rotates rect based on transform
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        INCLUDE PERFETTO MODULE android.winscope.rect;

        SELECT
          sfd.display_id, wtr.group_id, wtr.depth, wtr.is_visible, wtr.opacity, rect.x, rect.y, rect.w, rect.h
        FROM android_surfaceflinger_display AS sfd
        INNER JOIN android_winscope_trace_rect AS wtr
          ON sfd.trace_rect_id = wtr.id
          AND sfd.snapshot_id = 3
        INNER JOIN android_winscope_rect AS rect
          ON rect.id = wtr.rect_id
        """,
        out=Csv("""
        "display_id","group_id","depth","is_visible","opacity","x","y","w","h"
        1,0,0,0,"[NULL]",0.000000,0.000000,5.000000,5.000000
        2,1,1,0,"[NULL]",0.000000,0.000000,2.000000,2.000000
        3,2,2,0,"[NULL]",0.000000,0.000000,10.000000,5.000000
        4,3,3,0,"[NULL]",0.000000,0.000000,5.000000,10.000000
        5,2,4,0,"[NULL]",0.000000,0.000000,5.000000,10.000000
        """))

  def test_input_rects(self):
    # makes 1 as visible spy window
    # makes 2 as non-visible non-spy window
    # crops 3 to display bounds
    # makes 4 as visible window despite layer rect being non-visible
    # ignore 5 as no input rect
    # makes 6 with empty fill region
    # makes 7 with fill region
    # makes 8 with fill region in different layer stack
    # makes 9 with fill region despite no frame
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        INCLUDE PERFETTO MODULE android.winscope.rect;

        SELECT
          sfl.layer_id,
          wtr.group_id,
          wtr.depth,
          wtr.is_visible,
          wtr.is_spy,
          rect.x,
          rect.y,
          rect.w,
          rect.h,
          fill_region_rect.x as fr_x,
          fill_region_rect.y as fr_y,
          fill_region_rect.w as fr_w,
          fill_region_rect.h as fr_h
        FROM surfaceflinger_layer AS sfl
        INNER JOIN android_winscope_trace_rect AS wtr
          ON sfl.input_rect_id = wtr.id
        INNER JOIN android_winscope_rect AS rect
          ON rect.id = wtr.rect_id
        LEFT JOIN android_winscope_fill_region AS fill_region
          ON fill_region.trace_rect_id = sfl.input_rect_id
        LEFT JOIN android_winscope_rect AS fill_region_rect
          ON fill_region_rect.id = fill_region.rect_id

        WHERE sfl.snapshot_id = 3
        """,
        out=Csv("""
        "layer_id","group_id","depth","is_visible","is_spy","x","y","w","h","fr_x","fr_y","fr_w","fr_h"
        1,0,1,1,1,0.000000,0.000000,1.000000,1.000000,"[NULL]","[NULL]","[NULL]","[NULL]"
        2,0,2,0,0,0.000000,0.000000,2.000000,2.000000,"[NULL]","[NULL]","[NULL]","[NULL]"
        3,0,3,1,0,0.000000,0.000000,5.000000,5.000000,"[NULL]","[NULL]","[NULL]","[NULL]"
        4,1,1,1,0,0.000000,0.000000,5.000000,5.000000,"[NULL]","[NULL]","[NULL]","[NULL]"
        6,0,4,1,0,0.000000,0.000000,5.000000,5.000000,0.000000,0.000000,0.000000,0.000000
        7,0,5,1,0,0.000000,0.000000,5.000000,5.000000,0.000000,0.000000,2.000000,2.000000
        7,0,5,1,0,0.000000,0.000000,5.000000,5.000000,2.000000,2.000000,1.000000,1.000000
        8,1,2,1,0,0.000000,0.000000,5.000000,5.000000,0.000000,0.000000,2.000000,2.000000
        9,2,1,1,0,0.000000,10.000000,0.000000,0.000000,0.000000,0.000000,5.000000,10.000000
        """))
