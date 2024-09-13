#!/usr/bin/env python3
# Copyright (C) 2020 The Android Open Source Project
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

import argparse
import os
import subprocess
import shutil
import sys
import tempfile


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument(
      '--lemon',
      default=os.path.join(
          os.path.normpath('buildtools/sqlite_src/tool/lemon.c')))
  parser.add_argument(
      '--lemon-template',
      default=os.path.join(
          os.path.normpath('buildtools/sqlite_src/tool/lempar.c')))
  parser.add_argument(
      '--clang',
      default=os.path.join(
          os.path.normpath('buildtools/linux64/clang/bin/clang')))
  parser.add_argument(
      '--preprocessor-grammar',
      default=os.path.join(
          os.path.normpath(
              'src/trace_processor/perfetto_sql/preprocessor/preprocessor_grammar.y'
          )),
  )
  args = parser.parse_args()

  with tempfile.TemporaryDirectory() as tmp:
    subprocess.check_call([
        args.clang,
        os.path.join(args.lemon), '-o',
        os.path.join(tmp, 'lemon')
    ])
    shutil.copy(args.lemon_template, tmp)
    subprocess.check_call([
        os.path.join(tmp, 'lemon'),
        args.preprocessor_grammar,
        '-q',
        '-l',
        '-s',
    ])

  return 0


if __name__ == '__main__':
  sys.exit(main())
