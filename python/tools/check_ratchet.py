#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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
"""
Force the reduction in use of some methods/types over time.
Often a method ('LEGACY_registerTrackController') or a type ('any')
gets replaced by a better alternative ('registerTrack', 'unknown') and
we want to a. replace all existing uses, b. prevent the introduction of
new uses. This presubmit helps with both. It keeps a count of the
number of instances of "FOO" in the codebase. At presubmit time we run
the script. If the "FOO" count has gone up we encourage the author to
use the alternative. If the "FOO" count has gone down we congratulate
them and prompt them to reduce the expected count.
Since the number of "FOO"s can only go down eventually they will all
be gone - completing the migration.
See also https://qntm.org/ratchet.
"""

import sys
import os
import re
import argparse

ROOT_DIR = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UI_SRC_DIR = os.path.join(ROOT_DIR, 'ui', 'src')

EXPECTED_ANY_COUNT = 73
# 'any' is too generic. It will show up in many comments etc. So
# instead of counting any directly we forbid it using eslint and count
# the number of suppressions.
ANY_REGEX = r"// eslint-disable-next-line @typescript-eslint/no-explicit-any"


def all_source_files():
  for root, dirs, files in os.walk(UI_SRC_DIR, followlinks=False):
    for name in files:
      if name.endswith('.ts'):
        yield os.path.join(root, name)


def do_check(options):
  total_any_count = 0
  for path in all_source_files():
    with open(path) as f:
      s = f.read()
      any_count = len(re.findall(ANY_REGEX, s))
      total_any_count += any_count

  if total_any_count > EXPECTED_ANY_COUNT:
    print(f'More "{ANY_REGEX}" {EXPECTED_ANY_COUNT} -> {total_any_count}')
    print(
        f'  Expected to find {EXPECTED_ANY_COUNT} instances of "{ANY_REGEX}" accross the .ts & .d.ts files in the code base.'
    )
    print(f'  Instead found {total_any_count}.')
    print(f'  It it likely your CL introduces additional uses of "any".')
    return 1
  elif total_any_count < EXPECTED_ANY_COUNT:
    print(f'Less "{ANY_REGEX}" {EXPECTED_ANY_COUNT} -> {total_any_count}')
    print(
        f'  Congratulations your CL reduces the instances of "{ANY_REGEX}" in the code base from {EXPECTED_ANY_COUNT} to {total_any_count}.'
    )
    print(
        f'  Please go to {__file__} and set EXPECTED_ANY_COUNT to {total_any_count}.'
    )
    return 1

  return 0


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.set_defaults(func=do_check)
  subparsers = parser.add_subparsers()

  check_command = subparsers.add_parser(
      'check', help='Check the rules (default)')
  check_command.set_defaults(func=do_check)

  options = parser.parse_args()
  return options.func(options)


if __name__ == '__main__':
  sys.exit(main())
