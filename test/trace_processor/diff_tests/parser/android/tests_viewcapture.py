#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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


class ViewCapture(TestSuite):

  def test_snapshot_table(self):
    return DiffTestBlueprint(
        trace=Path('viewcapture.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.viewcapture;
        SELECT
          id, ts, package_name, window_name
        FROM
          android_viewcapture;
        """,
        out=Csv("""
        "id","ts","package_name","window_name"
        0,448881087865,"com.google.android.apps.nexuslauncher","[NULL]"
        1,448883575576,"com.google.android.apps.nexuslauncher","[NULL]"
        2,448883575876,"[NULL]","PhoneWindow"
        """))

  def test_snapshot_args(self):
    return DiffTestBlueprint(
        trace=Path('viewcapture.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.viewcapture;
        SELECT
          args.key, args.display_value
        FROM
          android_viewcapture AS vc JOIN args ON vc.arg_set_id = args.arg_set_id
        WHERE vc.id = 0
        ORDER BY args.key;
        """,
        out=Csv("""
        "key","display_value"
        "package_name","com.google.android.apps.nexuslauncher"
        "window_name","STRING DE-INTERNING ERROR"
        "window_name_iid","1"
        """))

  def test_view_table(self):
    return DiffTestBlueprint(
        trace=Path('viewcapture.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.viewcapture;
        SELECT
          id, snapshot_id, node_id, hashcode, is_visible, parent_id, view_id, class_name
        FROM
          android_viewcapture_view
        LIMIT 4;
        """,
        out=Csv("""
        "id","snapshot_id","node_id","hashcode","is_visible","parent_id","view_id","class_name"
        0,0,0,182652084,1,4294967295,"NO_ID","com.android.internal.policy.PhoneWindow@6cec234"
        1,0,1,130248925,0,0,"[NULL]","com.android.internal.policy.DecorView"
        2,1,0,182652084,1,4294967295,"NO_ID","com.android.internal.policy.PhoneWindow@6cec234"
        3,1,1,130248925,1,0,"TEST_VIEW_ID","[NULL]"
        """))

  def test_view_args(self):
    return DiffTestBlueprint(
        trace=Path('viewcapture.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.viewcapture;
        SELECT
          args.key, args.display_value
        FROM
          android_viewcapture_view AS vc JOIN args ON vc.arg_set_id = args.arg_set_id
          WHERE vc.snapshot_id = 1
        ORDER BY args.arg_set_id, args.key
        LIMIT 10;
        """,
        out=Csv("""
        "key","display_value"
        "alpha","1.0"
        "class_name","com.android.internal.policy.PhoneWindow@6cec234"
        "hashcode","182652084"
        "height","2400"
        "parent_id","-1"
        "scale_x","1.0"
        "scale_y","1.0"
        "view_id","NO_ID"
        "width","1080"
        "will_not_draw","true"
        """))

  def test_handle_string_deinterning_errors(self):
    return DiffTestBlueprint(
        trace=Path('viewcapture.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.viewcapture;
        SELECT
          args.key, args.display_value
        FROM
          android_viewcapture_view AS vc JOIN args ON vc.arg_set_id = args.arg_set_id
        WHERE args.key = 'class_name'
        ORDER BY display_value;
        """,
        out=Csv("""
        "key","display_value"
        "class_name","STRING DE-INTERNING ERROR"
        "class_name","com.android.internal.policy.DecorView"
        "class_name","com.android.internal.policy.PhoneWindow@6cec234"
        "class_name","com.android.internal.policy.PhoneWindow@6cec234"
        """))

  def test_tables_has_raw_protos(self):
    return DiffTestBlueprint(
        trace=Path('viewcapture.textproto'),
        query="""
        SELECT COUNT(*) FROM __intrinsic_viewcapture
        WHERE base64_proto_id IS NOT NULL
        UNION ALL
        SELECT COUNT(*) FROM __intrinsic_viewcapture_view
        WHERE base64_proto_id IS NOT NULL
        """,
        out=Csv("""
        "COUNT(*)"
        3
        13
        """))

  def test_view_rects(self):
    #             0             0: no shift/scroll/scale/translation
    #           /   \           1: change in width and height
    #          1     3          2: scale > 0, different alpha
    #        /         \        3: scroll > 0, no shift from parent
    #       2           4       4: inherits scroll and shift from parent
    #     /            / \      5: no scale, inherits scale from parent
    #    5            7   8     6: scale > 0. inherits scale from parent
    #   /                       7: translation_x > 0
    #  6                        8: translation_y > 0
    return DiffTestBlueprint(
        trace=Path('viewcapture.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.viewcapture;
        INCLUDE PERFETTO MODULE android.winscope.rect;

        SELECT
          vcv.node_id, wtr.group_id, wtr.depth, wtr.is_visible, wtr.opacity, rect.x, rect.y, rect.w, rect.h
        FROM android_viewcapture_view AS vcv
        INNER JOIN android_winscope_trace_rect AS wtr
          ON vcv.trace_rect_id = wtr.id
        INNER JOIN android_winscope_rect AS rect
          ON rect.id = wtr.rect_id
        WHERE vcv.snapshot_id = 2
        """,
        out=Csv("""
        "node_id","group_id","depth","is_visible","opacity","x","y","w","h"
        0,0,0,1,1.000000,0.000000,0.000000,1.000000,1.000000
        1,0,4,0,1.000000,0.000000,0.000000,1.000000,2.000000
        2,0,8,0,0.500000,-1.500000,-4.500000,6.000000,12.000000
        3,0,4,1,1.000000,0.000000,0.000000,3.000000,3.000000
        4,0,8,1,1.000000,-1.000000,-2.000000,3.000000,3.000000
        5,0,12,0,1.000000,-1.500000,-4.500000,6.000000,12.000000
        6,0,16,0,1.000000,-3.000000,-13.500000,9.000000,30.000000
        7,0,12,1,1.000000,3.000000,-2.000000,3.000000,3.000000
        8,0,12,1,1.000000,-1.000000,2.000000,3.000000,3.000000
        """))
