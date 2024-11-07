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


class InputMethodClients(TestSuite):

  def test_has_expected_rows(self):
    return DiffTestBlueprint(
        trace=Path('inputmethod_clients.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.inputmethod;
        SELECT
          id, ts
        FROM
          android_inputmethod_clients;
        """,
        out=Csv("""
        "id","ts"
        0,119232512509
        1,119237883196
        """))

  def test_has_expected_args(self):
    return DiffTestBlueprint(
        trace=Path('inputmethod_clients.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.inputmethod;
        SELECT
          args.key, args.display_value
        FROM
          android_inputmethod_clients AS imc JOIN args ON imc.arg_set_id = args.arg_set_id
        WHERE imc.id = 0
        ORDER BY args.key
        LIMIT 10;
        """,
        out=Csv("""
        "key","display_value"
        "client.editor_info.field_id","2131362278"
        "client.editor_info.ime_options","33554435"
        "client.editor_info.input_type","1"
        "client.editor_info.package_name","com.google.android.apps.nexuslauncher"
        "client.editor_info.private_ime_options","com.google.android.inputmethod.latin.appSupportsSmartComposeAndDel,com.google.android.inputmethod.latin.canary.appSupportsSmartComposeAndDel,com.google.android.inputmethod.latin.dev.appSupportsSmartComposeAndDel"
        "client.ime_focus_controller.has_ime_focus","true"
        "client.ime_insets_source_consumer.insets_source_consumer.has_window_focus","true"
        "client.ime_insets_source_consumer.insets_source_consumer.is_requested_visible","true"
        "client.ime_insets_source_consumer.insets_source_consumer.source_control.leash.hash_code","135479902"
        "client.ime_insets_source_consumer.insets_source_consumer.source_control.leash.layerId","105"
        """))

  def test_table_has_raw_protos(self):
    return DiffTestBlueprint(
        trace=Path('inputmethod_clients.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.winscope.inputmethod;
        SELECT COUNT(*) FROM android_inputmethod_clients
        WHERE base64_proto IS NOT NULL AND base64_proto_id IS NOT NULL
        """,
        out=Csv("""
        "COUNT(*)"
        2
        """))
