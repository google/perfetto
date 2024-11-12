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


class InputMethodService(TestSuite):

  def test_has_expected_rows(self):
    return DiffTestBlueprint(
        trace=Path('inputmethod_service.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.inputmethod;
        SELECT
          id, ts
        FROM
          android_inputmethod_service;
        """,
        out=Csv("""
        "id","ts"
        0,61829562285
        1,61831101307
        """))

  def test_has_expected_args(self):
    return DiffTestBlueprint(
        trace=Path('inputmethod_service.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.inputmethod;
        SELECT
          args.key, args.display_value
        FROM
          android_inputmethod_service AS ims JOIN args ON ims.arg_set_id = args.arg_set_id
        WHERE ims.id = 0
        ORDER BY args.key;
        """,
        out=Csv("""
        "key","display_value"
        "input_method_service.candidates_visibility","4"
        "input_method_service.configuration","{1.0 ?mcc0mnc [en_US] ldltr sw411dp w411dp h842dp 420dpi nrml long hdr widecg port night finger -keyb/v/h -nav/h winConfig={ mBounds=Rect(0, 0 - 1080, 2400) mAppBounds=Rect(0, 128 - 1080, 2337) mMaxBounds=Rect(0, 0 - 1080, 2400) mDisplayRotation=ROTATION_0 mWindowingMode=fullscreen mDisplayWindowingMode=fullscreen mActivityType=undefined mAlwaysOnTop=undefined mRotation=ROTATION_0} s.11 fontWeightAdjustment=0}"
        "input_method_service.input_binding","InputBinding{android.os.BinderProxy@cbb5415 / uid 10254 / pid 2812}"
        "input_method_service.input_editor_info.package_name","com.google.android.apps.nexuslauncher"
        "input_method_service.input_started","true"
        "input_method_service.last_computed_insets.content_top_insets","126"
        "input_method_service.last_computed_insets.touchable_insets","2"
        "input_method_service.last_computed_insets.touchable_region","SkRegion()"
        "input_method_service.settings_observer","SettingsObserver{mShowImeWithHardKeyboard=1}"
        "input_method_service.soft_input_window.window_state","2"
        "input_method_service.token","android.os.BinderProxy@50043d1"
        "where","InputMethodService#doFinishInput"
        """))

  def test_table_has_raw_protos(self):
    return DiffTestBlueprint(
        trace=Path('inputmethod_service.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.inputmethod;
        SELECT COUNT(*) FROM android_inputmethod_service
        WHERE base64_proto IS NOT NULL AND base64_proto_id IS NOT NULL
        """,
        out=Csv("""
        "COUNT(*)"
        2
        """))
