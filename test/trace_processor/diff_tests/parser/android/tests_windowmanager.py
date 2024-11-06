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


class WindowManager(TestSuite):

  def test_has_expected_rows(self):
    return DiffTestBlueprint(
        trace=Path('windowmanager.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.windowmanager;
        SELECT
          ts
        FROM
          android_windowmanager;
        """,
        out=Csv("""
        "ts"
        558296470731
        558884171862
        """))

  def test_has_expected_args(self):
    return DiffTestBlueprint(
        trace=Path('windowmanager.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.windowmanager;
        SELECT
          args.key, args.display_value
        FROM
          android_windowmanager AS vc JOIN args ON vc.arg_set_id = args.arg_set_id
        WHERE vc.id = 0
        ORDER BY args.key
        LIMIT 10;
        """,
        out=Csv("""
        "key","display_value"
        "elapsed_realtime_nanos","558296470731"
        "where","trace.enable"
        "window_manager_service.focused_app","com.google.android.apps.nexuslauncher/.NexusLauncherActivity"
        "window_manager_service.focused_window.hash_code","160447612"
        "window_manager_service.focused_window.title","com.google.android.apps.nexuslauncher/com.google.android.apps.nexuslauncher.NexusLauncherActivity"
        "window_manager_service.input_method_window.hash_code","217051718"
        "window_manager_service.input_method_window.title","InputMethod"
        "window_manager_service.policy.keyguard_delegate.interactive_state","INTERACTIVE_STATE_AWAKE"
        "window_manager_service.policy.keyguard_delegate.screen_state","SCREEN_STATE_ON"
        "window_manager_service.policy.keyguard_draw_complete","true"
        """))

  def test_table_has_raw_protos(self):
    return DiffTestBlueprint(
        trace=Path('windowmanager.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.windowmanager;
        SELECT COUNT(*) FROM android_windowmanager
        WHERE base64_proto IS NOT NULL AND base64_proto_id IS NOT NULL
        """,
        out=Csv("""
        "COUNT(*)"
        2
        """))
