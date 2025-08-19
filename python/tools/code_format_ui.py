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


class UICodeFormatter(CodeFormatterBase):

  def __init__(self, name: str, exts: list[str], tool: str):
    super().__init__(name=name, exts=exts)
    self.tool = tool

  def filter_files(self, files):
    # Filter based on extension first.
    filtered = super().filter_files(files)
    # Filter out changes outside of ui/
    return [f for f in filtered if f.startswith('ui/')]

  def create_tool_cmd(self, check_only: bool, files: list[str]) -> list[str]:
    raise NotImplementedError('Subclasses must implement this')

  def run_formatter(self, repo_root: str, check_only: bool, files: list[str]):
    tool = self.tool
    ui_dir = os.path.join(repo_root, 'ui')
    if not os.path.exists(os.path.join(ui_dir, tool)):
      err = f'Cannot find ${tool}\nRun tools/install-build-deps --ui'
      print(err, file=sys.stderr)
      return 127
    files = [os.path.relpath(os.path.abspath(f), ui_dir) for f in files]
    cmd = self.create_tool_cmd(check_only, files)
    return self.check_call(cmd, cwd=ui_dir, env=node_env(repo_root))

  def print_fix_hint(self):
    print('Run ui/format-sources to fix', file=sys.stderr)


class Prettier(UICodeFormatter):

  def __init__(self):
    super().__init__(
        name='prettier',
        exts=['.ts', '.js', '.scss'],
        tool='node_modules/.bin/prettier')

  def filter_files(self, files):
    # Start from the common UI filtering.
    filtered = super().filter_files(files)
    # Apply .prettierignore within ui/
    with open('ui/.prettierignore', 'r') as fd:
      ignorelist = fd.read().strip().split('\n')
      filtered = [
          f for f in filtered if not any(fnmatch(f, i) for i in ignorelist)
      ]
    return filtered

  def create_tool_cmd(self, check_only: bool, files: list[str]) -> list[str]:
    return [self.tool, '--log-level=warn'
           ] + ['--check' if check_only else '--write'] + files


class Eslint(UICodeFormatter):

  def __init__(self):
    super().__init__(
        name='eslint', exts=['.ts', '.js'], tool='node_modules/.bin/eslint')

  def create_tool_cmd(self, check_only: bool, files: list[str]) -> list[str]:
    return [self.tool] + ([] if check_only else ['--fix']) + files


UI_CODE_FORMATTERS = [Prettier(), Eslint()]

if __name__ == '__main__':
  sys.exit(run_code_formatters(UI_CODE_FORMATTERS))
