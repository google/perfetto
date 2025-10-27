#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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


class SurfaceFlingerLayers(TestSuite):

  def test_snapshot_table(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        SELECT
          id, ts, has_invalid_elapsed_ts
        FROM
          surfaceflinger_layers_snapshot LIMIT 3;
        """,
        out=Csv("""
        "id","ts","has_invalid_elapsed_ts"
        0,2748300281655,0
        1,2749500341063,0
        2,2749700000000,1
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
        "elapsed_realtime_nanos","123"
        "vsync_id","24766"
        "where","visibleRegionsDirty"
        """))

  def test_displays_table(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
        INCLUDE PERFETTO MODULE android.winscope.rect;

        SELECT
          dis.id,
          dis.snapshot_id,
          dis.is_on,
          dis.is_virtual,
          dis.display_id,
          dis.display_name,
          RR.x,
          RR.y,
          RR.w,
          RR.h
        FROM android_surfaceflinger_display as dis
          inner join android_winscope_trace_rect as TR
          on TR.id = dis.trace_rect_id
          inner join android_winscope_rect as RR
          on RR.id = TR.rect_id
          LIMIT 5;
        """,
        out=Csv("""
        "id","snapshot_id","is_on","is_virtual","display_id","display_name","x","y","w","h"
        0,0,1,0,4619827677550801152,"Common Panel",0.000000,0.000000,1080.000000,2400.000000
        1,1,1,1,4619827677550801152,"Common Panel",0.000000,0.000000,1080.000000,2400.000000
        2,1,0,0,4619827677550801153,"Common Panel",0.000000,0.000000,1080.000000,2400.000000
        3,2,1,0,1,"[NULL]",0.000000,0.000000,5.000000,5.000000
        4,2,1,0,2,"Display2",0.000000,0.000000,2.000000,2.000000
        """))

  def test_layer_table(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        SELECT
          id,
          snapshot_id,
          layer_id,
          layer_name,
          parent,
          corner_radius_tl,
          corner_radius_tr,
          corner_radius_bl,
          corner_radius_br,
          hwc_composition_type,
          z_order_relative_of,
          is_missing_z_parent,
          is_visible,
          layer_rect_id,
          input_rect_id
        FROM
          surfaceflinger_layer
        LIMIT 8;
        """,
        out=Csv("""
        "id","snapshot_id","layer_id","layer_name","parent","corner_radius_tl","corner_radius_tr","corner_radius_bl","corner_radius_br","hwc_composition_type","z_order_relative_of","is_missing_z_parent","is_visible","layer_rect_id","input_rect_id"
        0,0,3,"Display 0 name="Built-in Screen"#3","[NULL]",0.000000,0.000000,0.000000,0.000000,"[NULL]",5,1,0,"[NULL]","[NULL]"
        1,0,4,"WindowedMagnification:0:31#4",3,0.100000,0.000000,0.300000,0.400000,"[NULL]","[NULL]",0,0,"[NULL]","[NULL]"
        2,1,3,"Display 0 name="Built-in Screen"#3","[NULL]",0.000000,0.000000,0.000000,0.000000,"[NULL]","[NULL]",0,0,"[NULL]","[NULL]"
        3,1,4,"WindowedMagnification:0:31#4",3,0.000000,0.000000,0.000000,0.000000,"[NULL]","[NULL]",0,0,3,"[NULL]"
        4,2,"[NULL]","[NULL]",-1,0.000000,0.000000,0.000000,0.000000,"[NULL]","[NULL]",0,0,"[NULL]","[NULL]"
        5,2,-2,"[NULL]",-2,0.000000,0.000000,0.000000,0.000000,"[NULL]","[NULL]",0,0,"[NULL]","[NULL]"
        6,2,1,"layer1","[NULL]",1.000000,1.000000,1.000000,1.000000,2,"[NULL]",0,0,9,10
        7,2,2,"layer2","[NULL]",0.000000,0.000000,0.000000,0.000000,"[NULL]","[NULL]",0,1,11,12
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
        3
        20
        """))

  def test_layer_args(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_layers.textproto'),
        query="""
        SELECT
          args.key, args.display_value
        FROM
          surfaceflinger_layer AS sfl JOIN args ON sfl.arg_set_id = args.arg_set_id
        WHERE sfl.id = 2 AND (key GLOB "screen_bounds*" OR key GLOB "visibility_reason*")
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
          AND sfl.snapshot_id = 2
        INNER JOIN android_winscope_rect AS rect
          ON rect.id = wtr.rect_id
        """,
        out=Csv("""
        "layer_id","group_id","depth","is_visible","opacity","x","y","w","h"
        1,0,1,0,"[NULL]",0.000000,0.000000,1.000000,1.000000
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
          AND sfd.snapshot_id = 2
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

        WHERE sfl.snapshot_id = 2
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
