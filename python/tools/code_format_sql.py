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

from code_format_utils import CodeFormatterBase, run_code_formatters

IGNORE_DIRS = [
    'test/trace_processor/diff_tests/',
    'src/trace_processor/metrics/sql/',
    # The source of truth for the chrome stdlib is in chromium. Skip formatting
    # to avoid diverging from its upstream.
    'src/trace_processor/perfetto_sql/stdlib/chrome/',
]


class SqlGlot(CodeFormatterBase):

  def __init__(self):
    super().__init__(name='sqlglot', exts=['.sql'])

  def filter_files(self, files):
    # Filter based on extension first.
    filtered = super().filter_files(files)
    # Apply ignore list.
    filtered = [
        f for f in filtered if not any(f.startswith(i) for i in IGNORE_DIRS)
    ]
    return filtered

  def run_formatter(self, repo_root: str, check_only: bool, files: list[str]):
    if sys.platform != 'win32':
      venv_py = '.venv/bin/python3'
    else:
      venv_py = '.venv/Scripts/python3.exe'
    fmt_script = 'python/tools/format_sql.py'
    if not os.path.exists(venv_py):
      err = f'Cannot find ${venv_py}\nRun tools/install-build-deps'
      print(err, file=sys.stderr)
      return 127
    cmd = [venv_py, fmt_script]
    cmd += (['--check-only'] if check_only else ['--in-place'])
    cmd += files
    return self.check_call(cmd)

  def print_fix_hint(self):
    print('Run tools/format-sql-sources to fix', file=sys.stderr)


if __name__ == '__main__':
  sys.exit(run_code_formatters([SqlGlot()]))
