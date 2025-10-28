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


class WindowManager(TestSuite):

  def test_snapshot_has_expected_rows(self):
    return DiffTestBlueprint(
        trace=Path('windowmanager.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.windowmanager;
        SELECT
          ts, focused_display_id, has_invalid_elapsed_ts
        FROM
          android_windowmanager;
        """,
        out=Csv("""
        "ts","focused_display_id","has_invalid_elapsed_ts"
        558296470731,0,0
        558884171862,2,1
        """))

  def test_snapshot_has_expected_args(self):
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
        "elapsed_realtime_nanos","123"
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

  def test_snapshot_has_raw_proto(self):
    return DiffTestBlueprint(
        trace=Path('windowmanager.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.windowmanager;
        SELECT COUNT(*) FROM android_windowmanager
        WHERE base64_proto_id IS NOT NULL
        """,
        out=Csv("""
        "COUNT(*)"
        2
        """))

  def test_windowcontainer_has_expected_rows(self):
    return DiffTestBlueprint(
        trace=Path('windowmanager.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.windowmanager;
        SELECT COUNT(*)
        FROM android_windowmanager_windowcontainer
        WHERE snapshot_id = 0;
        """,
        out=Csv("""
        "COUNT(*)"
        70
        """))

  def test_windowcontainer_has_expected_args(self):
    return DiffTestBlueprint(
        trace=Path('windowmanager.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.windowmanager;
        SELECT args.key, args.display_value
        FROM android_windowmanager_windowcontainer wc
        INNER JOIN args ON wc.arg_set_id = args.arg_set_id
        WHERE snapshot_id = 0 AND wc.id = 0;
        """,
        out=Csv("""
        "key","display_value"
        "window_container.configuration_container.full_configuration.window_configuration.windowing_mode","1"
        "window_container.orientation","-2"
        "window_container.visible","true"
        "window_container.identifier.hash_code","64646999"
        "window_container.identifier.user_id","-10000"
        "window_container.identifier.title","WindowContainer"
        "keyguard_controller.keyguard_per_display[0]","[NULL]"
        "is_home_recents_component","true"
        """))

  def test_windowcontainer_has_rects(self):
    return DiffTestBlueprint(
        trace=Path('windowmanager.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.windowmanager;
        INCLUDE PERFETTO MODULE android.winscope.rect;
        SELECT x, y, w, h
        FROM android_windowmanager_windowcontainer wc
        INNER JOIN android_winscope_trace_rect tr ON wc.window_rect_id = tr.id
        INNER JOIN android_winscope_rect r ON tr.rect_id = r.id
        INNER JOIN android_winscope_transform t ON tr.transform_id = t.id
        WHERE snapshot_id = 0;
        """,
        out=Csv("""
        "x","y","w","h"
        0.000000,0.000000,1080.000000,2400.000000
        0.000000,0.000000,1080.000000,2400.000000
        0.000000,0.000000,1080.000000,2400.000000
        0.000000,0.000000,1080.000000,2400.000000
        120.000000,2274.000000,840.000000,126.000000
        0.000000,0.000000,1080.000000,128.000000
        0.000000,0.000000,1080.000000,2400.000000
        0.000000,2274.000000,1080.000000,126.000000
        402.000000,848.000000,276.000000,704.000000
        540.000000,1200.000000,0.000000,0.000000
        0.000000,0.000000,1080.000000,128.000000
        0.000000,2326.000000,1080.000000,74.000000
        """))
