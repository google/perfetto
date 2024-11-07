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

  def test_has_expected_rows(self):
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

  def test_has_expected_args(self):
    return DiffTestBlueprint(
        trace=Path('viewcapture.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.viewcapture;
        SELECT
          args.key, args.display_value
        FROM
          android_viewcapture AS vc JOIN args ON vc.arg_set_id = args.arg_set_id
        WHERE vc.id = 0
        ORDER BY args.key
        LIMIT 10;
        """,
        out=Csv("""
        "key","display_value"
        "package_name","com.google.android.apps.nexuslauncher"
        "views[0].alpha","1.0"
        "views[0].class_name","com.android.internal.policy.PhoneWindow@6cec234"
        "views[0].hashcode","182652084"
        "views[0].height","2400"
        "views[0].parent_id","-1"
        "views[0].scale_x","1.0"
        "views[0].scale_y","1.0"
        "views[0].view_id","NO_ID"
        "views[0].width","1080"
        """))

  def test_handle_string_deinterning_errors(self):
    return DiffTestBlueprint(
        trace=Path('viewcapture.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.viewcapture;
        SELECT
          args.key, args.display_value
        FROM
          android_viewcapture AS vc JOIN args ON vc.arg_set_id = args.arg_set_id
        WHERE vc.id = 1 and args.key = 'views[1].class_name';
        """,
        out=Csv("""
        "key","display_value"
        "views[1].class_name","STRING DE-INTERNING ERROR"
        """))

  def test_table_has_raw_protos(self):
    return DiffTestBlueprint(
        trace=Path('viewcapture.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.viewcapture;
        SELECT COUNT(*) FROM android_viewcapture
        WHERE base64_proto IS NOT NULL AND base64_proto_id IS NOT NULL
        """,
        out=Csv("""
        "COUNT(*)"
        2
        """))
