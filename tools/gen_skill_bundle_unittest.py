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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_skill_bundle  # pylint: disable=wrong-import-position

ROOT = Path(__file__).resolve().parent.parent
DROP_REPORTS = 'ai/skills/drop_reports'

SKILL_MD = """---
name: demo
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

## Reporting

Answer with numbers.
"""

BOTH_MD = """---
requires: [query]
related:
  - path: diff.md
    when: Two traces
---
# Both
trace_processor query --remote S --query-file $SKILL_ROOT/workflows/scripts/both.sql
Python: $SKILL_ROOT/workflows/scripts/tool.py
"""

DIFF_MD = """---
requires: [query, multi_trace]
---
# Diff
"""

CLI_ADAPTER = """---
provides: [query, multi_trace, python, write_file]
description: A demo skill for tests.
---
CLI setup text.

CLI report text.
"""

# A test-only host that provides only query.
LIMITED_ADAPTER = """---
provides: [query]
description: A limited demo.
---
Limited setup text.
"""

UI_ADAPTER = """---
provides: [query]
description: >-
  A demo skill
  for the UI.
---
UI setup text.
"""


class GenSkillBundleTest(unittest.TestCase):

  def setUp(self):
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
    """Builds with the in-tree adapter <host>.md, or with no adapter."""
    args = ['--skill', str(self.src), '--out', str(self.out)]
    if host:
      args += ['--adapter', str(self.src / f'environment-references/{host}.md')]
    return gen_skill_bundle.main(args + list(extra))

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
    self.assertIn(
        '## Environment setup\n\nCLI setup text.\n\nCLI report text.\n\n'
        '## Workflows\n', skill)
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

  def test_adapter_from_another_location(self):
    other = Path(self.tmp.name) / 'google3.md'
    other.write_text(
        '---\nprovides: [query]\ndescription: Google3.\n---\nGoogle3 text.\n')
    self.assertEqual(self._main(None, '--adapter', str(other)), 0)
    skill = (self.out / 'demo/SKILL.md').read_text()
    self.assertIn('Google3 text.', skill)
    self.assertNotIn('Limited setup text.', skill)
    self.assertNotIn('workflows/diff.md', self._tree())
    # A skill with adapters needs --adapter, and the file must exist.
    self.assertEqual(self._main(None), 1)
    self.assertEqual(self._main('nowhere'), 1)

  def test_skill_without_adapters(self):
    # Release tags from before the adapters: the description is in
    # SKILL-template.md, which links a setup file with no frontmatter, and the
    # content files have no requires.
    for name in ['cli.md', 'limited.md']:
      (self.src / 'environment-references' / name).unlink()
    self._write('environment-references/setup.md', '# Setup\n')
    self._write('workflows/both.md', BOTH_MD.replace('requires: [query]\n', ''))
    template = SKILL_MD.replace(
        'name: demo\n', 'name: demo\ndescription: Demo.\n').replace(
            '{{environment_setup}}\n',
            '[setup]($SKILL_ROOT/environment-references/'
            'setup.md)\n')
    self._write('SKILL-template.md', template)
    # Without adapters, a file cannot require an action.
    self.assertEqual(self._main(None), 1)
    self._write('workflows/diff.md', '# Diff\n')
    self.assertEqual(self._main(None), 0)
    self.assertIn('environment-references/setup.md', self._tree())
    self.assertIn('description: Demo.\n',
                  (self.out / 'demo/SKILL.md').read_text())
    # The description is needed, and a slot needs an adapter.
    self._write('SKILL-template.md', template.replace('description: Demo.\n',
                                                      ''))
    self.assertEqual(self._main(None), 1)
    self._write('SKILL-template.md', template + '{{environment_setup}}\n')
    self.assertEqual(self._main(None), 1)

  def test_actions_are_open_tags(self):
    # A file can need an action that no in-tree adapter provides.
    self._write('workflows/diff.md', DIFF_MD.replace('multi_trace', 'fly'))
    report = Path(self.tmp.name) / 'drops.txt'
    self.assertEqual(self._main('cli', '--drop-report', str(report)), 0)
    self.assertIn('\nworkflows/diff.md: fly\n', report.read_text())
    self._write('environment-references/flying.md',
                LIMITED_ADAPTER.replace('[query]', '[query, fly]'))
    self.assertEqual(self._main('flying'), 0)
    self.assertIn('workflows/diff.md', self._tree())
    # Query is an action like the others: a file that needs nothing reaches
    # an adapter that provides nothing.
    self._write('environment-references/flying.md',
                LIMITED_ADAPTER.replace('[query]', '[]'))
    self._write('workflows/both.md', BOTH_MD.replace('[query]', '[]'))
    self.assertEqual(self._main('flying'), 0)
    self.assertIn('workflows/both.md', self._tree())

  def test_description_comes_from_the_adapter(self):
    self.assertEqual(self._main('cli'), 0)
    self.assertTrue((self.out / 'demo/SKILL.md').read_text().startswith(
        '---\nname: demo\ndescription: A demo skill for tests.\n---\n'))
    self.assertEqual(self._main('limited'), 0)
    self.assertIn('description: A limited demo.\n',
                  (self.out / 'demo/SKILL.md').read_text())
    # With adapters, a description in SKILL-template.md is an error.
    self._write(
        'SKILL-template.md',
        SKILL_MD.replace('name: demo\n', 'name: demo\ndescription: Demo.\n'))
    self.assertEqual(self._main('cli'), 1)

  def test_bad_references_fail(self):
    for ref in [
        '[it](x.md)', '[it](../../outside.md)', '$SKILL_ROOT/workflows/x.sql',
        '[it]($SKILL_ROOT/environment-references/cli.md)'
    ]:
      with self.subTest(ref=ref):
        self._write('workflows/both.md', BOTH_MD + f'See {ref}.\n')
        self.assertEqual(self._main('cli'), 1)

  def test_link_to_a_dropped_file_fails(self):
    self._write('workflows/both.md',
                BOTH_MD + 'See [diff]($SKILL_ROOT/workflows/diff.md).\n')
    self.assertEqual(self._main('cli'), 0)
    self.assertEqual(self._main('limited'), 1)

  def test_bad_frontmatter_fails(self):
    cases = {
        'unknown key':
            '---\nrequires: [query]\nneeds: [python]\n---\n# X\n',
        'no requires':
            '# X\n',
        'requires not a list':
            '---\nrequires: query\n---\n# X\n',
        'related to a missing file': ('---\nrequires: [query]\nrelated:\n'
                                      '  - path: x.md\n    when: X\n---\n'
                                      '# X\n'),
        'related without when': ('---\nrequires: [query]\nrelated:\n'
                                 '  - path: diff.md\n---\n'),
        'related as a map': ('---\nrequires: [query]\nrelated:\n'
                             '  path: diff.md\n  when: X\n---\n'),
    }
    # Link other.md, so that only its frontmatter can fail the build.
    self._write('workflows/both.md', BOTH_MD + 'See [it](other.md).\n')
    for name, text in cases.items():
      with self.subTest(name):
        self._write('workflows/other.md', text)
        self.assertEqual(self._main('cli'), 1)

  def test_unreached_file_fails(self):
    self._write('workflows/orphan.md',
                '---\nrequires: [query]\n---\n# Orphan\n')
    for layout in ['tree', 'index']:
      with self.subTest(layout=layout):
        self.assertEqual(self._main('cli', '--layout', layout), 1)
    (self.src / 'workflows/orphan.md').unlink()
    # A file that only a dropped file links to needs the same requires.
    self._write('workflows/diff.md', DIFF_MD + 'See [it](helper.md).\n')
    self._write('workflows/helper.md',
                '---\nrequires: [query]\n---\n# Helper\n')
    self.assertEqual(self._main('cli'), 0)
    self.assertEqual(self._main('limited'), 1)
    self._write('workflows/helper.md', DIFF_MD)
    self.assertEqual(self._main('limited'), 0)

  def test_bad_adapters_fail(self):
    cases = {
        'not a list':
            ('limited.md', LIMITED_ADAPTER.replace('[query]', 'query')),
        'no provides': ('limited.md',
                        LIMITED_ADAPTER.replace('provides: [query]\n', '')),
        'no description':
            ('limited.md',
             LIMITED_ADAPTER.replace('description: A limited demo.\n', '')),
        'description too long':
            ('limited.md', LIMITED_ADAPTER.replace('A limited demo.',
                                                   'x' * 1025)),
    }
    for name, (rel, text) in cases.items():
      with self.subTest(name):
        self._write(f'environment-references/{rel}', text)
        self.assertEqual(self._main('limited'), 1)
        self._write('environment-references/limited.md', LIMITED_ADAPTER)

  def test_adapter_body_fills_the_setup_slot(self):
    # The body is copied as it is, also text that looks like a slot.
    code = '~~~\n{{environment_setup}}\n~~~\n'
    self._write('environment-references/limited.md',
                LIMITED_ADAPTER + '\n' + code)
    self.assertEqual(self._main('limited'), 0)
    skill = (self.out / 'demo/SKILL.md').read_text()
    self.assertIn('## Environment setup\n\nLimited setup text.\n\n' + code,
                  skill)

  def test_bad_slots_fail(self):
    # No slot line for the adapter, and a slot line used twice.
    for text in [
        SKILL_MD.replace('{{environment_setup}}\n', ''),
        SKILL_MD.replace('{{environment_setup}}\n',
                         '{{environment_setup}}\n{{environment_setup}}\n'),
    ]:
      with self.subTest(text=text):
        self._write('SKILL-template.md', text)
        self.assertEqual(self._main('cli'), 1)

  def test_ui_index_and_reachability(self):
    self._write('environment-references/ui.md', UI_ADAPTER)
    self.assertEqual(self._main('ui', '--layout', 'index'), 0)
    (entry,) = json.loads((self.out / 'index.json').read_text())['skills']
    self.assertEqual(entry['name'], 'demo')
    self.assertEqual(entry['description'], 'A demo skill for the UI.')
    self.assertTrue((self.out / 'demo/SKILL.md').read_text().startswith(
        '---\nname: demo\ndescription: >-\n  A demo skill\n  for the UI.\n---\n'
    ))
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
    self._write('workflows/a+b.md', '---\nrequires: [query]\n---\n# A+B\n')
    self._write('workflows/both.md',
                BOTH_MD + 'See [it]($SKILL_ROOT/workflows/a+b.md).\n')
    self.assertEqual(self._main('cli'), 0)
    self.assertEqual(self._main('ui', '--layout', 'index'), 1)

  def test_layout_does_not_depend_on_the_host(self):
    self._write('environment-references/ui.md', UI_ADAPTER)
    self.assertEqual(self._main('ui'), 0)
    self.assertIn('workflows/scripts/tool.py', self._tree())
    self.assertFalse((self.out / 'index.json').exists())
    # The host still decides the files: the CLI index has diff.md.
    self.assertEqual(self._main('cli', '--layout', 'index'), 0)
    (entry,) = json.loads((self.out / 'index.json').read_text())['skills']
    self.assertIn('workflows/diff.md', [f['path'] for f in entry['files']])

  def test_read_only_source(self):
    """A read-only source, as in a build sandbox, gives a writable tree."""
    if os.geteuid() == 0:
      self.skipTest('root ignores file permissions')
    self._write('bin/tool', '#!/bin/sh\n')
    (self.src / 'bin/tool').chmod(0o755)
    paths = [self.src, *self.src.rglob('*')]
    for p in paths:
      p.chmod(p.stat().st_mode & ~0o222)
    # Runs before the temporary directory is removed.
    self.addCleanup(lambda: [p.chmod(p.stat().st_mode | 0o200) for p in paths])
    self.assertEqual(self._main('cli'), 0)
    tree = self.out / 'demo'
    for rel in ('SKILL.md', 'workflows/both.md', 'workflows/scripts/both.sql'):
      self.assertTrue(os.access(tree / rel, os.W_OK), rel)
    self.assertTrue(os.access(tree / 'bin/tool', os.X_OK))
    self.assertFalse(os.access(tree / 'workflows/scripts/both.sql', os.X_OK))

  def test_in_tree_skill_builds(self):
    # A skill change that breaks a host's bundle fails here, before the build.
    skill = ROOT / 'ai/skills/perfetto'
    cli = skill / 'environment-references/cli.md'
    ui = skill / 'environment-references/ui.md'
    cli_out = self.out / 'cli'
    self.assertEqual(
        gen_skill_bundle.main([
            '--adapter',
            str(cli), '--skill',
            str(skill), '--out',
            str(cli_out)
        ]), 0)
    self.assertTrue((cli_out / 'perfetto/SKILL.md').is_file())
    self.assertFalse((cli_out / 'perfetto/environment-references').exists())
    self.assertNotIn('{{', (cli_out / 'perfetto/SKILL.md').read_text())
    # The CLI provides every action, so it gets every file.
    self.assertEqual(gen_skill_bundle.Skill(skill, cli).dropped, {})

    # The command of RFC-0052: the skill is the in-tree one by default.
    ui_out = self.out / 'ui'
    self.assertEqual(
        gen_skill_bundle.main(
            ['--adapter',
             str(ui), '--layout', 'index', '--out',
             str(ui_out)]), 0)
    (entry,) = json.loads((ui_out / 'index.json').read_text())['skills']
    paths = {f['path'] for f in entry['files']}
    self.assertIn('workflows/gpu/gpu_info.md', paths)
    self.assertIn('workflows/gpu/scripts/gpu_info.sql', paths)
    self.assertFalse(
        any(p.startswith('environment-references/') for p in paths))
    self.assertFalse(any(p.endswith('.py') for p in paths))

  def test_drop_reports_are_up_to_date(self):
    skill = ROOT / 'ai/skills/perfetto'
    adapters = gen_skill_bundle.Adapter.find_in(skill)
    self.assertEqual(
        sorted(p.name for p in (ROOT / DROP_REPORTS).iterdir()),
        sorted(Path(rel).stem + '.txt' for rel in adapters))
    for rel in adapters:
      name = Path(rel).stem
      with self.subTest(name):
        report = self.out / f'{name}.txt'
        self.assertEqual(
            gen_skill_bundle.main([
                '--adapter',
                str(skill / rel), '--out',
                str(self.out / name), '--drop-report',
                str(report)
            ]), 0)
        self.assertEqual(
            report.read_text(),
            (ROOT / DROP_REPORTS / f'{name}.txt').read_text(),
            f'regenerate {DROP_REPORTS}/{name}.txt with '
            f'tools/gen_skill_bundle.py --adapter ai/skills/perfetto/{rel} '
            f'--drop-report {DROP_REPORTS}/{name}.txt --out <tmp>')


if __name__ == '__main__':
  unittest.main()
