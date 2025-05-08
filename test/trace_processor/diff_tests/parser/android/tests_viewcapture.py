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
          id, ts
        FROM
          android_viewcapture;
        """,
        out=Csv("""
        "id","ts"
        0,448881087865
        1,448883575576
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
        SELECT
          id, snapshot_id
        FROM
          __intrinsic_viewcapture_view;
        """,
        out=Csv("""
        "id","snapshot_id"
        0,0
        1,0
        2,1
        3,1
        """))

  def test_view_args(self):
    return DiffTestBlueprint(
        trace=Path('viewcapture.textproto'),
        query="""
        SELECT
          args.key, args.display_value
        FROM
          __intrinsic_viewcapture_view AS vc JOIN args ON vc.arg_set_id = args.arg_set_id
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
        SELECT
          args.key, args.display_value
        FROM
          __intrinsic_viewcapture_view AS vc JOIN args ON vc.arg_set_id = args.arg_set_id
        WHERE args.key = 'class_name';
        """,
        out=Csv("""
        "key","display_value"
        "class_name","com.android.internal.policy.PhoneWindow@6cec234"
        "class_name","com.android.internal.policy.PhoneWindow@6cec234"
        "class_name","com.android.internal.policy.DecorView"
        "class_name","STRING DE-INTERNING ERROR"
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
        2
        4
        """))
