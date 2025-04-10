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


class GnFormat(CodeFormatterBase):

  def __init__(self):
    super().__init__(name='gn', exts=['.gn', '.gni'])

  def run_formatter(self, repo_root: str, check_only: bool, files: list[str]):
    tool = 'tools/gn'
    if not os.path.exists(tool):
      err = f'Cannot find ${tool}\nRun tools/install-build-deps'
      print(err, file=sys.stderr)
      return 127
    cmd = [tool, 'format'] + (['--dry-run'] if check_only else []) + files
    return self.check_call(cmd)

  def print_fix_hint(self):
    print('Run tools/format-gn-sources to fix', file=sys.stderr)


if __name__ == '__main__':
  sys.exit(run_code_formatters([GnFormat()]))
