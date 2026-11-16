#!/usr/bin/env python3
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
"""Tests for tools/gen_skill_bundle.py.

Run: python3 tools/gen_skill_bundle_unittest.py
"""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_skill_bundle  # pylint: disable=wrong-import-position

ROOT = Path(__file__).resolve().parent.parent
UI_DROP_REPORT = 'ai/skills/drop_reports/ui.txt'

SKILL_MD = """---
name: demo
description: >-
  A demo skill
  for tests.
---

# Demo

## Environment setup

{{environment_setup}}

## Workflows

1. **One trace**
   - Both hosts: [both.md]($SKILL_ROOT/workflows/both.md)

2. **Two traces**
   - Diff: [diff.md]($SKILL_ROOT/workflows/diff.md)

3. **Docs**: [proto](/protos/x.proto), [site](https://perfetto.dev)
"""

BOTH_MD = """---
related:
  - path: diff.md
    when: Two traces
---
# Both
trace_processor query --remote S --query-file $SKILL_ROOT/workflows/scripts/both.sql
Python: $SKILL_ROOT/workflows/scripts/tool.py
"""

DIFF_MD = """---
requires: [multi_trace]
---
# Diff
"""

CLI_ADAPTER = """---
host: cli
provides: [query, multi_trace, python]
---
CLI setup text.
"""

# A test-only host that provides only the baseline action.
LIMITED_ADAPTER = """---
host: limited
provides: [query]
---
Limited setup text.
"""

UI_ADAPTER = """---
host: ui
provides: [query]
---
UI setup text.
"""


class GenSkillBundleTest(unittest.TestCase):

  def setUp(self):
    patcher = mock.patch.object(gen_skill_bundle, 'HOSTS',
                                gen_skill_bundle.HOSTS + ('limited',))
    patcher.start()
    self.addCleanup(patcher.stop)
    self.tmp = tempfile.TemporaryDirectory()
    self.addCleanup(self.tmp.cleanup)
    self.src = Path(self.tmp.name) / 'demo'
    self.out = Path(self.tmp.name) / 'out'
    self._write('SKILL-template.md', SKILL_MD)
    self._write('OWNERS', 'someone\n')
    self._write('environment-references/cli.md', CLI_ADAPTER)
    self._write('environment-references/limited.md', LIMITED_ADAPTER)
    self._write('workflows/both.md', BOTH_MD)
    self._write('workflows/diff.md', DIFF_MD)
    self._write('workflows/scripts/both.sql', 'SELECT 1;\n')
    self._write('workflows/scripts/tool.py', 'print(1)\n')

  def _write(self, rel, text):
    path = self.src / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)

  def _main(self, host, *extra):
    return gen_skill_bundle.main(
        ['--host', host, '--skill',
         str(self.src), '--out',
         str(self.out)] + list(extra))

  def _tree(self):
    dst = self.out / 'demo'
    return sorted(
        p.relative_to(dst).as_posix() for p in dst.rglob('*') if p.is_file())

  def test_cli_tree(self):
    self.assertEqual(self._main('cli'), 0)
    self.assertEqual(self._tree(), [
        'SKILL.md',
        'workflows/both.md',
        'workflows/diff.md',
        'workflows/scripts/both.sql',
        'workflows/scripts/tool.py',
    ])
    skill = (self.out / 'demo/SKILL.md').read_text()
    self.assertIn('## Environment setup\n\nCLI setup text.\n\n## Workflows\n',
                  skill)
    self.assertNotIn('Limited setup', skill)
    self.assertNotIn('{{', skill)
    self.assertIn(
        '2. **Two traces**\n'
        '   - Diff: [diff.md]($SKILL_ROOT/workflows/diff.md)\n', skill)
    # Frontmatter is not copied; related entries become a section.
    both = (self.out / 'demo/workflows/both.md').read_text()
    self.assertTrue(both.startswith('# Both\n'))
    self.assertIn(
        '\n## Related workflows\n\n'
        '- Two traces: [diff.md]($SKILL_ROOT/workflows/diff.md)\n', both)
    self.assertEqual((self.out / 'demo/workflows/diff.md').read_text(),
                     '# Diff\n')

  def test_files_follow_the_actions_of_the_host(self):
    report = Path(self.tmp.name) / 'drops.txt'
    self.assertEqual(self._main('limited', '--drop-report', str(report)), 0)
    self.assertIn('\nworkflows/diff.md: multi_trace\n', report.read_text())
    self.assertNotIn('workflows/diff.md', self._tree())
    skill = (self.out / 'demo/SKILL.md').read_text()
    self.assertIn('## Environment setup\n\nLimited setup text.\n', skill)
    # The emptied group is removed and the list is numbered again.
    self.assertIn(
        '1. **One trace**\n'
        '   - Both hosts: [both.md]($SKILL_ROOT/workflows/both.md)\n\n'
        '2. **Docs**', skill)
    both = (self.out / 'demo/workflows/both.md').read_text()
    self.assertNotIn('Related workflows', both)

  def test_adapter_flag_replaces_the_skill_adapter(self):
    other = Path(self.tmp.name) / 'other.md'
    other.write_text('---\nhost: limited\nprovides: [query]\n---\n'
                     'Other text.\n')
    self.assertEqual(self._main('limited', '--adapter', str(other)), 0)
    skill = (self.out / 'demo/SKILL.md').read_text()
    self.assertIn('Other text.', skill)
    self.assertNotIn('Limited setup text.', skill)
    # An adapter for another host is refused.
    self.assertEqual(self._main('cli', '--adapter', str(other)), 1)

  def test_skill_without_adapters(self):
    for name in ['cli.md', 'limited.md']:
      (self.src / 'environment-references' / name).unlink()
    # Without adapters, a file cannot require an action.
    self.assertEqual(self._main('cli'), 1)
    self._write('workflows/diff.md', '# Diff\n')
    self._write('SKILL-template.md',
                SKILL_MD.replace('{{environment_setup}}\n', ''))
    self.assertEqual(self._main('cli'), 0)
    # A slot with no adapter is an error.
    self._write('SKILL-template.md', SKILL_MD)
    self.assertEqual(self._main('cli'), 1)

  def test_bad_references_fail(self):
    for ref in [
        '[it](x.md)', '[it](../../outside.md)', '$SKILL_ROOT/workflows/x.sql',
        '[it]($SKILL_ROOT/environment-references/cli.md)'
    ]:
      for host in ['cli', 'limited']:
        with self.subTest(ref=ref, host=host):
          self._write('workflows/both.md', BOTH_MD + f'See {ref}.\n')
          self.assertEqual(self._main(host), 1)

  def test_link_to_a_dropped_file_fails(self):
    self._write('workflows/both.md',
                BOTH_MD + 'See [diff]($SKILL_ROOT/workflows/diff.md).\n')
    self.assertEqual(self._main('cli'), 0)
    self.assertEqual(self._main('limited'), 1)

  def test_bad_frontmatter_fails(self):
    cases = {
        'unknown key': '---\nneeds: [python]\n---\n# X\n',
        'unknown action': '---\nrequires: [fly]\n---\n# X\n',
        'related to a missing file': ('---\nrelated:\n  - path: x.md\n'
                                      '    when: X\n---\n# X\n'),
        'related without when': '---\nrelated:\n  - path: diff.md\n---\n',
    }
    for name, text in cases.items():
      with self.subTest(name):
        self._write('workflows/other.md', text)
        self.assertEqual(self._main('cli'), 1)

  def test_bad_adapters_fail(self):
    cases = {
        'unknown host':
            ('limited.md', '---\nhost: web\nprovides: [query]\n---\n'),
        'unknown action': ('limited.md', '---\nhost: limited\n'
                           'provides: [query, fly]\n---\n'),
        'no query': ('limited.md', '---\nhost: limited\nprovides: [python]\n'
                     '---\n'),
        'not a list': ('limited.md', '---\nhost: limited\nprovides: query\n'
                       '---\n'),
        'same host twice': ('limited2.md', LIMITED_ADAPTER),
    }
    for name, (rel, text) in cases.items():
      with self.subTest(name):
        self._write(f'environment-references/{rel}', text)
        self.assertEqual(self._main('limited'), 1)
        self._write('environment-references/limited.md', LIMITED_ADAPTER)
        (self.src /
         'environment-references/limited2.md').unlink(missing_ok=True)

  def test_router_needs_one_slot(self):
    for text in [
        SKILL_MD.replace('{{environment_setup}}\n', ''),
        SKILL_MD.replace('{{environment_setup}}\n',
                         '{{environment_setup}}\n{{environment_setup}}\n'),
    ]:
      with self.subTest(text=text):
        self._write('SKILL-template.md', text)
        self.assertEqual(self._main('cli'), 1)

  def test_no_adapter_for_the_host_fails(self):
    (self.src / 'environment-references/limited.md').unlink()
    self.assertEqual(self._main('limited'), 1)

  def test_ui_index_and_reachability(self):
    self._write('environment-references/ui.md', UI_ADAPTER)
    self.assertEqual(self._main('ui'), 0)
    (entry,) = json.loads((self.out / 'index.json').read_text())['skills']
    self.assertEqual(entry['name'], 'demo')
    self.assertEqual(entry['description'], 'A demo skill for tests.')
    self.assertEqual([f['path'] for f in entry['files']], [
        'SKILL.md',
        'workflows/both.md',
        'workflows/scripts/both.sql',
    ])
    for f in entry['files']:
      self.assertEqual(f['bytes'],
                       (self.out / 'demo' / f['path']).stat().st_size)
    skill = (self.out / 'demo/SKILL.md').read_text()
    self.assertIn('## Environment setup\n\nUI setup text.\n', skill)

  def test_ui_rejects_paths_the_ui_cannot_read(self):
    self._write('environment-references/ui.md', UI_ADAPTER)
    self._write('workflows/a+b.md', '# A+B\n')
    self._write('workflows/both.md',
                BOTH_MD + 'See [it]($SKILL_ROOT/workflows/a+b.md).\n')
    self.assertEqual(self._main('cli'), 0)
    self.assertEqual(self._main('ui'), 1)

  def test_extension_server_layout(self):
    self._write('environment-references/ui.md', UI_ADAPTER)
    self.assertEqual(
        self._main('ui', '--module', 'default', '--replaces', 'demo'), 0)
    mod = self.out / 'modules' / 'default'
    (entry,) = json.loads((mod / 'skills').read_text())['skills']
    self.assertEqual(entry['replaces'], 'demo')
    self.assertTrue((mod / 'skill_files/demo/SKILL.md').is_file())
    self.assertTrue((mod / 'skill_files/demo/workflows/both.md').is_file())
    # Module ids are path segments of the server URL.
    for module, rc in [('memory-tools', 0), ('a/b', 1), ('..', 1)]:
      with self.subTest(module=module):
        self.assertEqual(self._main('ui', '--module', module), rc)
    # --module and --replaces are for the UI host, and --replaces needs a
    # module.
    self.assertEqual(self._main('cli', '--module', 'default'), 1)
    self.assertEqual(self._main('ui', '--replaces', 'demo'), 1)

  def test_upstream_perfetto_skill_builds(self):
    # A skill change that breaks a host's bundle fails here, before the build.
    skill = ROOT / 'ai/skills/perfetto'
    cli_out = self.out / 'cli'
    self.assertEqual(
        gen_skill_bundle.main(
            ['--host', 'cli', '--skill',
             str(skill), '--out',
             str(cli_out)]), 0)
    self.assertTrue((cli_out / 'perfetto/SKILL.md').is_file())
    self.assertFalse((cli_out / 'perfetto/environment-references').exists())
    self.assertNotIn('{{', (cli_out / 'perfetto/SKILL.md').read_text())
    # The CLI provides every action, so it gets every file.
    self.assertEqual(gen_skill_bundle.Skill(skill, 'cli').dropped, {})

    ui_out = self.out / 'ui'
    report = self.out / 'drops.txt'
    self.assertEqual(
        gen_skill_bundle.main([
            '--host', 'ui', '--skill',
            str(skill), '--out',
            str(ui_out), '--drop-report',
            str(report)
        ]), 0)
    (entry,) = json.loads((ui_out / 'index.json').read_text())['skills']
    paths = {f['path'] for f in entry['files']}
    self.assertIn('workflows/gpu/gpu_info.md', paths)
    self.assertIn('workflows/gpu/scripts/gpu_info.sql', paths)
    self.assertFalse(
        any(p.startswith('environment-references/') for p in paths))
    self.assertFalse(any(p.endswith('.py') for p in paths))
    # The checked-in drop report is up to date.
    self.assertEqual(
        report.read_text(), (ROOT / UI_DROP_REPORT).read_text(),
        f'regenerate {UI_DROP_REPORT} with tools/gen_skill_bundle.py '
        '--host ui --skill ai/skills/perfetto --drop-report '
        f'{UI_DROP_REPORT} --out <tmp>')


if __name__ == '__main__':
  unittest.main()
