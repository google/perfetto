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


class InputMethodManagerService(TestSuite):

  def test_has_expected_rows(self):
    return DiffTestBlueprint(
        trace=Path('inputmethod_manager_service.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.inputmethod;
        SELECT
          id, ts
        FROM
          android_inputmethod_manager_service;
        """,
        out=Csv("""
        "id","ts"
        0,39998329771
        1,40003054136
        """))

  def test_has_expected_args(self):
    return DiffTestBlueprint(
        trace=Path('inputmethod_manager_service.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.inputmethod;
        SELECT
          args.key, args.display_value
        FROM
          android_inputmethod_manager_service AS imms JOIN args ON imms.arg_set_id = args.arg_set_id
        WHERE imms.id = 0
        ORDER BY args.key;
        """,
        out=Csv("""
        "key","display_value"
        "input_method_manager_service.bound_to_method","true"
        "input_method_manager_service.cur_attribute.package_name","com.google.android.apps.nexuslauncher"
        "input_method_manager_service.cur_client","ClientState{6c16bdd mUid=10254 mPid=2790 mSelfReportedDisplayId=0}"
        "input_method_manager_service.cur_focused_window_name","a74954b com.google.android.apps.nexuslauncher/com.google.android.apps.nexuslauncher.NexusLauncherActivity"
        "input_method_manager_service.cur_focused_window_soft_input_mode","STATE_UNSPECIFIED|ADJUST_NOTHING|IS_FORWARD_NAVIGATION"
        "input_method_manager_service.cur_id","com.google.android.inputmethod.latin/com.android.inputmethod.latin.LatinIME"
        "input_method_manager_service.cur_method_id","com.google.android.inputmethod.latin/com.android.inputmethod.latin.LatinIME"
        "input_method_manager_service.cur_seq","1"
        "input_method_manager_service.cur_token","android.os.Binder@3abdbc4"
        "input_method_manager_service.have_connection","true"
        "input_method_manager_service.is_interactive","true"
        "input_method_manager_service.last_ime_target_window_name","a74954b com.google.android.apps.nexuslauncher/com.google.android.apps.nexuslauncher.NexusLauncherActivity"
        "input_method_manager_service.system_ready","true"
        "where","InputMethodManagerService#startInputOrWindowGainedFocus"
        """))
