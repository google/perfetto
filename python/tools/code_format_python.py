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

IGNORE_PATHS = [
    # Auto-generated Python protos.
    'python/perfetto/protos/perfetto/trace/perfetto_trace_pb2.py',
    # The source of truth of Chrome's stdlib is in Chromium. The copy in
    # Perfetto is imported via Copybara. Don't format it to avoid diverging from
    # Chromium.
    'test/trace_processor/diff_tests/stdlib/chrome/',
]


def is_python_exec_script(file):
  if not os.path.isfile(file):
    return False
  with open(file, 'r') as f:
    return f.readline().startswith('#!/usr/bin/env python')


class Yapf(CodeFormatterBase):

  def __init__(self):
    super().__init__(name='yapf', exts=['.py'])

  def filter_files(self, files):
    # Filter based on extension first.
    filtered = super().filter_files(files)
    # However some executable python script in tools are extensionless.
    filtered += [
        f for f in files if f.startswith('tools/') and is_python_exec_script(f)
    ]
    # Apply ignore list.
    filtered = [
        f for f in filtered if not any(f.startswith(i) for i in IGNORE_PATHS)
    ]
    return filtered

  def run_formatter(self, repo_root: str, check_only: bool, files: list[str]):
    if sys.platform != 'win32':
      tool = '.venv/bin/yapf'
    else:
      tool = '.venv/Scripts/yapf.exe'
    if not os.path.exists(tool):
      err = f'Cannot find ${tool}\nRun tools/install-build-deps'
      print(err, file=sys.stderr)
      return 127
    cmd = [tool, '--parallel', ('--quiet' if check_only else '-i')] + files
    return self.check_call(cmd)

  def print_fix_hint(self):
    print('Run tools/format-python-sources to fix', file=sys.stderr)


if __name__ == '__main__':
  sys.exit(run_code_formatters([Yapf()]))
