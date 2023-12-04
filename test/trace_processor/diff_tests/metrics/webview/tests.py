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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class WebView(TestSuite):

  def test_webview_jank_approximation_metric(self):
    return DiffTestBlueprint(
        trace=DataPath('webview_jank.pb'),
        query=Metric('webview_jank_approximation'),
        out=TextProto(r"""
            [perfetto.protos.webview_jank_approximation] {
                webview_janks: 4
                webview_janks_without_startup: 4
                webview_app_janks: 12
                webview_total_janks: 4
                total_janks: 12
            }
            """))
