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

from code_format_utils import ROOT_DIR, CodeFormatterBase, run_code_formatters

RUSTUP_HOME = os.path.join(ROOT_DIR, 'buildtools/rustup')
CARGO_HOME = os.path.join(ROOT_DIR, '.cargo')


class RustFormat(CodeFormatterBase):

  def __init__(self):
    super().__init__(name='rustfmt', exts=['.rs'])

  def run_formatter(self, repo_root: str, check_only: bool, files: list[str]):
    tool = '.cargo/bin/rustfmt'
    if not os.path.exists(tool):
      err = f'Cannot find {tool}\nRun tools/install-build-deps --rust'
      print(err, file=sys.stderr)
      return 127
    cmd = [tool, '--edition', '2024', '--unstable-features', '--skip-children']
    if check_only:
      cmd += ['--check']
    cmd += files
    env = os.environ.copy()
    env['RUSTUP_HOME'] = RUSTUP_HOME
    env['CARGO_HOME'] = CARGO_HOME
    return self.check_call(cmd, env=env)

  def print_fix_hint(self):
    print('Run tools/format-rust-sources to fix', file=sys.stderr)


if __name__ == '__main__':
  sys.exit(run_code_formatters([RustFormat()]))
