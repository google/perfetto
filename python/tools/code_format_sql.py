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
import platform
import sys

from code_format_utils import CodeFormatterBase, run_code_formatters

IGNORE_DIRS = [
    'test/trace_processor/diff_tests/',
    'src/trace_processor/metrics/sql/',
    # The source of truth for the chrome stdlib is in chromium. Skip formatting
    # to avoid diverging from its upstream.
    'src/trace_processor/perfetto_sql/stdlib/chrome/',
]

DIALECT_TARGET = 'perfetto_fmt_dialect'


def _syntaqlite_binary(repo_root: str) -> str:
  suffix = '.exe' if platform.system() == 'Windows' else ''
  return os.path.join(repo_root, 'buildtools', 'syntaqlite',
                      f'syntaqlite{suffix}')


def _dialect_library(out_dir: str) -> str:
  # GN's shared_library emits `libperfetto_fmt_dialect.so` on Linux/macOS and
  # `perfetto_fmt_dialect.dll` on Windows.
  if platform.system() == 'Windows':
    return os.path.join(out_dir, f'{DIALECT_TARGET}.dll')
  return os.path.join(out_dir, f'lib{DIALECT_TARGET}.so')


def _resolve_out_dir(repo_root: str) -> str:
  env_out = os.environ.get('OUT')
  if env_out:
    return env_out if os.path.isabs(env_out) else os.path.join(
        repo_root, env_out)
  out_root = os.path.join(repo_root, 'out')
  if not os.path.isdir(out_root):
    return ''
  candidates = [
      os.path.join(out_root, d)
      for d in os.listdir(out_root)
      if os.path.isfile(os.path.join(out_root, d, 'build.ninja'))
  ]
  if not candidates:
    return ''
  return max(candidates, key=os.path.getmtime)


class SyntaqliteFmt(CodeFormatterBase):

  def __init__(self):
    super().__init__(name='syntaqlite', exts=['.sql'])

  def filter_files(self, files):
    filtered = super().filter_files(files)
    filtered = [
        f for f in filtered if not any(f.startswith(i) for i in IGNORE_DIRS)
    ]
    return filtered

  def run_formatter(self, repo_root: str, check_only: bool, files: list[str]):
    binary = _syntaqlite_binary(repo_root)
    if not os.path.isfile(binary):
      print(
          f'syntaqlite binary not found at {binary}\n'
          'Run `tools/install-build-deps` to fetch it.',
          file=sys.stderr)
      return 127

    out_dir = _resolve_out_dir(repo_root)
    if not out_dir:
      print(
          'No GN out/ directory found. Run `tools/gn gen out/<config>` first.',
          file=sys.stderr)
      return 127

    # Keep the dialect library in sync with perfetto.y / perfetto.synq.
    ninja = os.path.join(repo_root, 'tools', 'ninja')
    rc = self.check_call([ninja, '-C', out_dir, DIALECT_TARGET])
    if rc != 0:
      return rc

    lib = _dialect_library(out_dir)
    if not os.path.isfile(lib):
      print(f'Dialect library not found at {lib}', file=sys.stderr)
      return 127

    cmd = [
        binary,
        'fmt',
        '--dialect',
        lib,
        '--dialect-name',
        'perfetto',
    ]
    cmd.append('--check' if check_only else '--in-place')
    cmd += files
    return self.check_call(cmd)

  def print_fix_hint(self):
    print('Run tools/format-sql-sources to fix', file=sys.stderr)


if __name__ == '__main__':
  sys.exit(run_code_formatters([SyntaqliteFmt()]))
