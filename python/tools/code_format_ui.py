#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

import os
import sys

from fnmatch import fnmatch
from code_format_utils import CodeFormatterBase, run_code_formatters


def node_env(repo_root):
  """Returns a modified env var set with tools/ prepended (for node)"""
  env = os.environ.copy()
  env['PATH'] = os.path.join(repo_root, 'tools') + os.pathsep + env['PATH']
  return env


class Prettier(CodeFormatterBase):

  def __init__(self):
    super().__init__(name='prettier', exts=['.ts', '.js', '.scss'])

  def filter_files(self, files):
    # Filter based on extension first.
    filtered = super().filter_files(files)
    # Filter out changes outside of ui/
    filtered = [f for f in filtered if f.startswith('ui/')]
    with open('ui/.prettierignore', 'r') as fd:
      ignorelist = fd.read().strip().split('\n')
      filtered = [
          f for f in filtered if not any(fnmatch(f, i) for i in ignorelist)
      ]
    return filtered

  def run_formatter(self, repo_root: str, check_only: bool, files: list[str]):
    tool = 'node_modules/.bin/prettier'
    ui_dir = os.path.join(repo_root, 'ui')
    if not os.path.exists(os.path.join(ui_dir, tool)):
      err = f'Cannot find ${tool}\nRun tools/install-build-deps --ui'
      print(err, file=sys.stderr)
      return 127
    files = [os.path.relpath(os.path.abspath(f), ui_dir) for f in files]
    cmd = [tool, '--log-level=warn'] + ['--check' if check_only else '--write']
    cmd += files
    return self.check_call(cmd, cwd=ui_dir, env=node_env(repo_root))

  def print_fix_hint(self):
    print('Run ui/format-sources to fix', file=sys.stderr)


class Eslint(CodeFormatterBase):

  def __init__(self):
    super().__init__(name='eslint', exts=['.ts', '.js'])

  def filter_files(self, files):
    # Filter based on extension first.
    filtered = super().filter_files(files)
    # Filter out changes outside of ui/
    filtered = [f for f in filtered if f.startswith('ui/')]
    return filtered

  def run_formatter(self, repo_root: str, check_only: bool, files: list[str]):
    out_ui_dir = os.path.join(repo_root, 'out', 'ui')
    if not os.path.exists(out_ui_dir):
      # Eslint requires all source dependencies to exist (e.g.: the .pbjs files),
      # so a UI build needs to be run first.
      print(f'Cannot run eslint because there is no UI build in {out_ui_dir}')
      return 127
    tool = 'node_modules/.bin/eslint'
    ui_dir = os.path.join(repo_root, 'ui')
    if not os.path.exists(os.path.join(ui_dir, tool)):
      err = f'Cannot find ${tool}\nRun tools/install-build-deps --ui'
      print(err, file=sys.stderr)
      return 127
    files = [os.path.relpath(os.path.abspath(f), ui_dir) for f in files]
    cmd = [tool] + ([] if check_only else ['--fix']) + files
    return self.check_call(cmd, cwd=ui_dir, env=node_env(repo_root))

  def print_fix_hint(self):
    print('Run ui/format-sources to fix', file=sys.stderr)


UI_CODE_FORMATTERS = [Prettier(), Eslint()]

if __name__ == '__main__':
  sys.exit(run_code_formatters(UI_CODE_FORMATTERS))
