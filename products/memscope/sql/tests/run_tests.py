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
"""Golden tests for the memscope PerfettoSQL package.

tools/diff_test_trace_processor.py has no way to add a package to a single
test, so each <name>.sql here is run with
`trace_processor_shell --add-sql-package` on the trace named in its first
line (`-- trace: <path from repo root>`) and compared with <name>.out.

Usage: run_tests.py <trace_processor_shell> [--rebaseline]
"""

import argparse
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../../..'))
TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
PACKAGE = os.path.join(ROOT, 'products/memscope/sql/memscope')


def run(tp, sql_path):
  with open(sql_path) as f:
    first = f.readline()
  assert first.startswith('-- trace: '), sql_path
  trace = os.path.join(ROOT, first[len('-- trace: '):].strip())
  res = subprocess.run(
      [tp, '--add-sql-package', PACKAGE, '-q', sql_path, trace],
      capture_output=True,
      text=True)
  if res.returncode != 0:
    return None, res.stderr
  return res.stdout, res.stderr


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('trace_processor_shell')
  parser.add_argument('--rebaseline', action='store_true')
  args = parser.parse_args()
  failed = 0
  names = sorted(f[:-4] for f in os.listdir(TESTS_DIR) if f.endswith('.sql'))
  for name in names:
    out, err = run(args.trace_processor_shell,
                   os.path.join(TESTS_DIR, name + '.sql'))
    golden = os.path.join(TESTS_DIR, name + '.out')
    if out is None:
      print(f'[FAIL] {name}\n{err}')
      failed += 1
      continue
    if args.rebaseline:
      with open(golden, 'w') as f:
        f.write(out)
      print(f'[REBASELINED] {name}')
      continue
    with open(golden) as f:
      expected = f.read()
    if out != expected:
      print(f'[FAIL] {name}\nExpected:\n{expected}\nActual:\n{out}')
      failed += 1
    else:
      print(f'[ OK ] {name}')
  print(f'{len(names) - failed}/{len(names)} passed')
  return 1 if failed else 0


if __name__ == '__main__':
  sys.exit(main())
