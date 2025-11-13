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
"""Common functions shared by the various tools/format-xxx code formatters"""

import argparse
import os
import subprocess

ROOT_DIR = os.path.dirname(
    os.path.dirname(os.path.abspath(os.path.dirname(__file__))))


class CodeFormatterBase:

  def __init__(self, name, exts=[]):
    self.name = name
    self.exts = exts
    self.parser = CodeFormatterBase.create_argparser()
    self.add_custom_args(self.parser)

  @staticmethod
  def create_argparser():
    parser = argparse.ArgumentParser()
    parser.add_argument('--check-only', action='store_true')
    parser.add_argument('--quiet', action='store_true')
    parser.add_argument('--upstream', type=str, default=None)
    parser.add_argument(
        '--all',
        action='store_true',
        help='Format all versioned sources, not just the changed ones')
    parser.add_argument('filelist', nargs='*')
    parser.add_argument(
        '--skip',
        type=str,
        default='',
        help='Comma-separated list of formatter names to skip')
    return parser

  def add_custom_args(self, parser):
    """Derived classes can override this to add custom args"""
    pass

  def filter_files(self, files: list[str]) -> list[str]:
    """Derived classes can override the file filtering"""
    return [f for f in files if any(f.endswith(ext) for ext in self.exts)]

  def run_formatter(self, repo_root: str, check_only: bool,
                    files: list[str]) -> int:
    """Invokes the formatter tool for the given files"""
    raise NotImplementedError('Subclasses must implement this')

  def print_fix_hint(self):
    pass

  @staticmethod
  def build_file_list(args):
    if args.all:
      # Case 1: the user passed --all and wants to format all the files.
      # List all the versioned sources known by git (i.e. no buildtools, etc).
      cmd = ['git', 'ls-files']
      return subprocess.check_output(cmd, text=True).splitlines()

    if len(args.filelist) > 0:
      # Case 2: the user explicitly passed a list of files to format.
      return args.filelist

    # Case 3 (most common): the user passed nothing and wants to format only
    # the changed files from the upstream branch (origin/main if unspecified).
    upstream_branch = args.upstream
    if upstream_branch is None or upstream_branch == '':
      try:
        cmd = [
            'git', 'rev-parse', '--abbrev-ref', '--symbolic-full-name', '@{u}'
        ]
        res = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL)
        upstream_branch = res.strip()
      except subprocess.CalledProcessError:
        upstream_branch = 'origin/main'
    cmd = ['git', 'diff', '--name-only', '--diff-filter=crd', upstream_branch]
    return subprocess.check_output(cmd, text=True).strip().splitlines()

  def check_call(self, cmd, env=None, **kwargs):
    try:
      subprocess.check_call(cmd, env=env, **kwargs)
      return 0
    except subprocess.CalledProcessError as ex:
      print('`%s` returned %d' % (' '.join(cmd)[:128], ex.returncode))
      return ex.returncode


def run_code_formatters(formatters: list[CodeFormatterBase]):
  parser = CodeFormatterBase.create_argparser()
  for formatter in formatters:
    formatter.add_custom_args(parser)
  args = parser.parse_args()
  # Make all passed paths (if any) relative to the REPO_ROOT.
  args.filelist = [
      os.path.relpath(os.path.abspath(x), ROOT_DIR) for x in args.filelist
  ]
  os.chdir(ROOT_DIR)
  files = CodeFormatterBase.build_file_list(args)
  skip_list = set(args.skip.split(','))
  formatters = [f for f in formatters if f.name not in skip_list]
  for formatter in formatters:
    files_to_check = formatter.filter_files(files)
    if not args.quiet:
      print(f'{formatter.name}: Formatting {len(files_to_check)} files')
    if len(files_to_check) == 0:
      continue
    res = formatter.run_formatter(ROOT_DIR, args.check_only, files_to_check)
    if res != 0:
      formatter.print_fix_hint()
      return res
