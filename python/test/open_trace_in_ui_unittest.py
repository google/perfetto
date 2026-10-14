# Copyright (C) 2026 The Android Open Source Project
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

from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path
import unittest

ROOT_DIR = Path(__file__).resolve().parents[2]
_SPEC = spec_from_loader(
    'open_trace_in_ui',
    SourceFileLoader('open_trace_in_ui',
                     str(ROOT_DIR / 'tools/open_trace_in_ui')))
open_trace_mod = module_from_spec(_SPEC)
_SPEC.loader.exec_module(open_trace_mod)


class OpenTraceInUiTest(unittest.TestCase):

  def test_parse_host(self):
    self.assertEqual(
        open_trace_mod._parse_host('http://myhost:8080'),
        ('http://myhost:8080/', 8080))
    self.assertEqual(
        open_trace_mod._parse_host('myhost:8080'),
        ('http://myhost:8080/', 8080))
    self.assertEqual(
        open_trace_mod._parse_host('myhost'), ('http://myhost:9001/', 9001))
    self.assertEqual(
        open_trace_mod._parse_host('https://proxy.example.com'),
        ('https://proxy.example.com/', 9001))

  def test_load_trace_launcher_html(self):
    rendered = open_trace_mod._load_trace_launcher_html(
        'https://ui.perfetto.dev/', 'trace "1".pftrace')
    self.assertIn(
        'data-ui-url="https://ui.perfetto.dev/#!/?referrer=open_trace_in_ui"',
        rendered)
    self.assertIn('data-fname="trace &quot;1&quot;.pftrace"', rendered)
    self.assertNotIn('{{UI_URL}}', rendered)
    self.assertNotIn('{{FNAME}}', rendered)


if __name__ == '__main__':
  unittest.main()
